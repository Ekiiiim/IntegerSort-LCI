#pragma once

#include <lci-irregular/detail/am_message.hpp>
#include <lci-irregular/detail/am_send_buffer.hpp>
#include <lci-irregular/detail/profiling.hpp>

#include <lci.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lci_irregular {
namespace detail {

struct WorkerBufferCacheKey {
  uint64_t state_generation = 0;
  size_t worker_index = 0;

  bool matches(uint64_t generation, size_t index) const noexcept {
    return state_generation == generation && worker_index == index;
  }
};

template <typename Record> size_t choose_batch_records(AmOptions options) {
  size_t max_bcopy = lci::get_max_bcopy_size();
  size_t max_records = max_bcopy / sizeof(Record);
  if (max_records == 0) {
    throw std::invalid_argument("LCI eager AM size cannot fit one record of the requested type");
  }
  if (options.batch_records == 0) {
    return max_records;
  }
  if (options.batch_records > max_records) {
    throw std::invalid_argument("requested AM batch size exceeds the maximum for this record type");
  }
  return options.batch_records;
}

class AmRuntimeStateBase {
public:
  virtual ~AmRuntimeStateBase() = default;

  virtual void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback) noexcept = 0;
  virtual void flush_worker(size_t worker_index) = 0;
  virtual void quiet_worker(size_t worker_index) = 0;
  virtual bool has_buffered_records() const = 0;
  virtual bool local_sends_drained() const noexcept = 0;
  virtual size_t received_record_count() const noexcept = 0;
  virtual AmProfile profile() const = 0;
  virtual void finalize_profile(IrregularRuntime& runtime) = 0;
};

template <typename Record, typename AmHandler> class TypedAmRuntimeState : public AmRuntimeStateBase {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(std::is_trivially_default_constructible<Record>::value,
                "Record must be trivially default constructible under C++17");
  static_assert(alignof(Record) <= alignof(std::max_align_t),
                "Record alignment greater than max_align_t is not supported");
  static_assert(std::is_invocable<AmHandler&, AmRecords<Record>>::value ||
                    std::is_invocable<AmHandler&, AmRecords<Record>, int>::value,
                "AM handler must accept AmRecords<Record>, with an optional source-rank argument");

  struct WorkerBuffers {
    WorkerBuffers(size_t batch_records, int rank_count, size_t bytes_per_buffer, bool use_upacket)
        : send_buffers(make_send_buffers(batch_records, rank_count)) {
      if (!use_upacket) {
        const size_t storage_bytes = bytes_per_buffer * static_cast<size_t>(rank_count);
        fallback_storage = std::malloc(storage_bytes);
        if (fallback_storage == nullptr && storage_bytes != 0) {
          throw std::bad_alloc();
        }
      }
    }

    ~WorkerBuffers() {
      std::free(fallback_storage);
    }

    WorkerBuffers(const WorkerBuffers&) = delete;
    WorkerBuffers& operator=(const WorkerBuffers&) = delete;

    std::vector<SendBuffer<Record>> send_buffers;
    void* fallback_storage = nullptr;
  };

  struct WorkerBufferCacheEntry {
    WorkerBufferCacheKey key;
    WorkerBuffers* buffers = nullptr;

    bool matches(uint64_t generation, size_t worker_index) const noexcept {
      return buffers != nullptr && key.matches(generation, worker_index);
    }
  };

public:
  TypedAmRuntimeState(IrregularRuntime& runtime, AmHandler am_handler, AmOptions options, uint64_t cache_generation)
      : runtime_(&runtime), am_handler_(std::move(am_handler)),
        profile_(runtime.profiling_enabled(), runtime.profiling_worker_count(), 0, options.profile_name),
        options_(std::move(options)), batch_records_(choose_batch_records<Record>(options_)),
        bytes_per_buffer_(message_bytes<Record>(batch_records_)), cache_generation_(cache_generation) {
    send_counter_ = lci::alloc_counter();
    try {
      lci::counter_set(send_counter_, 0);
    } catch (...) {
      lci::free_comp(&send_counter_);
      throw;
    }
  }

  ~TypedAmRuntimeState() override {
    if (!send_counter_.is_empty()) {
      lci::free_comp(&send_counter_);
    }
  }

  TypedAmRuntimeState(const TypedAmRuntimeState&) = delete;
  TypedAmRuntimeState& operator=(const TypedAmRuntimeState&) = delete;

  void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback) noexcept override {
    try {
      receive_message_impl(source_rank, message, message_bytes, loopback);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "LCI irregular receive callback terminated after exception: %s\n", error.what());
      std::terminate();
    } catch (...) {
      std::fprintf(stderr, "LCI irregular receive callback terminated after unknown exception\n");
      std::terminate();
    }
  }

  void post_record(size_t worker_index, int dest_rank, const Record& record) {
    if (dest_rank < 0 || dest_rank >= runtime_->rank_count()) {
      throw std::invalid_argument("LCI irregular AM destination rank is out of range");
    }

    WorkerBuffers& buffers = worker_buffers(worker_index);
    void* fallback_buffer = nullptr;
    if (!options_.use_upacket) {
      fallback_buffer = byte_offset(buffers.fallback_storage, static_cast<size_t>(dest_rank) * bytes_per_buffer_);
    }

    SendBuffer<Record>& send_buffer = buffers.send_buffers[static_cast<size_t>(dest_rank)];
    ProfileWorkerScope worker_scope(profile_.enabled(), worker_index);
    send_buffer.push(record, device_for_worker(worker_index), fallback_buffer);
    if (send_buffer.size() == send_buffer.capacity()) {
      flush_dest(buffers, worker_index, dest_rank);
    }
  }

  void flush_worker(size_t worker_index) override {
    WorkerBuffers& buffers = worker_buffers(worker_index);
    const int rank = runtime_->rank();
    const int rank_count = runtime_->rank_count();
    for (int i = 0; i < rank_count; ++i) {
      flush_dest(buffers, worker_index, (rank + i) % rank_count);
    }
  }

  void quiet_worker(size_t worker_index) override {
    while (!local_sends_drained()) {
      runtime_->progress(worker_index);
    }
  }

  bool local_sends_drained() const noexcept override {
    return static_cast<size_t>(lci::counter_get(send_counter_)) >= posted_am_count_.load(std::memory_order_relaxed);
  }

  bool has_buffered_records() const override {
    std::lock_guard<std::mutex> lock(worker_buffers_mutex_);
    for (const auto& worker : worker_buffers_) {
      if (!worker) {
        continue;
      }
      for (const auto& send_buffer : worker->send_buffers) {
        if (!send_buffer.empty()) {
          return true;
        }
      }
    }
    return false;
  }

  size_t received_record_count() const noexcept override {
    return received_record_count_.load(std::memory_order_relaxed);
  }

  AmProfile profile() const override {
    return profile_.snapshot();
  }

  void finalize_profile(IrregularRuntime& runtime) override {
    std::call_once(profile_finalize_once_, [this, &runtime] {
      if (profile_.enabled()) {
        submit_profile(runtime, profile_.snapshot());
      }
    });
  }

private:
  static std::vector<SendBuffer<Record>> make_send_buffers(size_t batch_records, int rank_count) {
    std::vector<SendBuffer<Record>> send_buffers;
    send_buffers.reserve(static_cast<size_t>(rank_count));
    for (int rank = 0; rank < rank_count; ++rank) {
      send_buffers.emplace_back(batch_records);
    }
    return send_buffers;
  }

  void receive_message_impl(int source_rank, const void* message, size_t message_bytes, bool loopback) {
    const size_t record_count = detail::record_count_from_message_bytes<Record>(message_bytes);

    profile_.measure_receive(loopback, record_count, record_count * sizeof(Record),
                             [&] { invoke_am_handler(AmRecords<Record>(message, record_count), source_rank); });
    received_record_count_.fetch_add(record_count, std::memory_order_relaxed);
  }

  void invoke_am_handler(AmRecords<Record> records, int source_rank) {
    if constexpr (std::is_invocable<AmHandler&, AmRecords<Record>, int>::value) {
      am_handler_(records, source_rank);
    } else {
      am_handler_(records);
    }
  }

  WorkerBuffers& worker_buffers(size_t worker_index) {
    WorkerBufferCacheEntry& cache = worker_buffer_cache();
    if (cache.matches(cache_generation_, worker_index)) {
      return *cache.buffers;
    }

    std::lock_guard<std::mutex> lock(worker_buffers_mutex_);
    if (worker_index >= worker_buffers_.size()) {
      worker_buffers_.resize(worker_index + 1);
    }
    std::unique_ptr<WorkerBuffers>& buffers = worker_buffers_[worker_index];
    if (!buffers) {
      buffers = std::make_unique<WorkerBuffers>(batch_records_, runtime_->rank_count(), bytes_per_buffer_,
                                                options_.use_upacket);
    }
    cache.key = WorkerBufferCacheKey{cache_generation_, worker_index};
    cache.buffers = buffers.get();
    return *cache.buffers;
  }

  static WorkerBufferCacheEntry& worker_buffer_cache() {
    static thread_local WorkerBufferCacheEntry cache;
    return cache;
  }

  const lci::device_t& device_for_worker(size_t worker_index) const {
    const auto& runtime_devices = devices(*runtime_);
    return runtime_devices[worker_index % runtime_devices.size()];
  }

  void flush_dest(WorkerBuffers& buffers, size_t worker_index, int dest_rank) {
    SendBuffer<Record>& send_buffer = buffers.send_buffers[static_cast<size_t>(dest_rank)];
    if (send_buffer.empty()) {
      return;
    }

    const size_t record_count = send_buffer.size();
    const size_t payload_bytes = record_count * sizeof(Record);
    profile_.measure(ProfileOperation::flush, worker_index, record_count, payload_bytes, [&] {
      ProfileWorkerScope worker_scope(profile_.enabled(), worker_index);
      post_send_buffer(dest_rank, send_buffer, runtime_->rank(), device_for_worker(worker_index), options_.use_loopback,
                       options_.use_upacket, send_counter_, remote_completion(*runtime_), *this, posted_am_count_);
    });
  }

  IrregularRuntime* runtime_;
  AmHandler am_handler_;
  AmProfileRecorder profile_;
  AmOptions options_;
  size_t batch_records_ = 0;
  size_t bytes_per_buffer_ = 0;
  lci::comp_t send_counter_ = nullptr;
  std::atomic<size_t> posted_am_count_{0};
  std::atomic<size_t> received_record_count_{0};
  mutable std::once_flag profile_finalize_once_;
  mutable std::mutex worker_buffers_mutex_;
  std::vector<std::unique_ptr<WorkerBuffers>> worker_buffers_;
  const uint64_t cache_generation_;
};

template <typename Record, typename AmHandler>
void post_record_erased(AmRuntimeStateBase& state, size_t worker_index, int dest_rank, const void* record) {
  auto& typed_state = static_cast<TypedAmRuntimeState<Record, AmHandler>&>(state);
  typed_state.post_record(worker_index, dest_rank, *static_cast<const Record*>(record));
}

} // namespace detail

template <typename Record, typename AmHandler>
void IrregularRuntime::set_am_handler(AmHandler am_handler, AmOptions options) {
  clear_am_handler();
  const bool use_upacket = options.use_upacket;
  const uint64_t cache_generation = allocate_am_cache_generation();
  auto state = std::make_unique<detail::TypedAmRuntimeState<Record, AmHandler>>(*this, std::move(am_handler),
                                                                                std::move(options), cache_generation);
  am_state_ = std::move(state);
  am_record_type_ = detail::record_type_identity<Record>();
  post_am_dispatch_ = &detail::post_record_erased<Record, AmHandler>;
  try {
    register_am_transport(use_upacket);
    barrier();
  } catch (...) {
    release_am_transport();
    am_state_.reset();
    clear_am_dispatch();
    throw;
  }
}

template <typename Record> void IrregularRuntime::post_am(size_t worker_index, int dest_rank, const Record& record) {
  validate_worker_index(worker_index);
  detail::AmRuntimeStateBase& state = require_am_state();
  if (!am_record_type_.matches(typeid(Record)) || post_am_dispatch_ == nullptr) {
    throw std::logic_error("LCI irregular AM record type does not match the active handler");
  }
  post_am_dispatch_(state, worker_index, dest_rank, &record);
}

} // namespace lci_irregular
