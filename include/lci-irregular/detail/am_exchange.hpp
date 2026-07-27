#pragma once

#include <lci-irregular/detail/am_exchange_state.hpp>
#include <lci-irregular/detail/profiling.hpp>

#include <lci.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lci_irregular {
namespace detail {

[[noreturn]] inline void terminate_current_exception(const char* context) noexcept {
  try {
    throw;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s after exception: %s\n", context, error.what());
  } catch (...) {
    std::fprintf(stderr, "%s after unknown exception\n", context);
  }
  std::terminate();
}

inline size_t align_up(size_t value, size_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

template <typename Record> size_t header_bytes() {
  return align_up(sizeof(AmMessageHeader), alignof(Record));
}

template <typename Record> size_t message_bytes(size_t batch_records) {
  return header_bytes<Record>() + batch_records * sizeof(Record);
}

template <typename Record> void write_message_header(void* buffer, size_t record_count) {
  if (record_count > std::numeric_limits<uint32_t>::max()) {
    std::fprintf(stderr, "LCI irregular AM batch has too many records: %zu\n", record_count);
    std::abort();
  }

  AmMessageHeader header{static_cast<uint32_t>(record_count)};
  std::memcpy(buffer, &header, sizeof(header));
}

inline void progress_device(lci::device_t device) {
  lci::progress_x().device(device)();
}

template <typename Record> void* allocate_upacket_blocking(lci::device_t device) {
  void* buffer = lci::get_upacket();
  while (buffer == nullptr) {
    progress_device(device);
    buffer = lci::get_upacket();
  }
  return buffer;
}

inline void* byte_offset(void* ptr, size_t offset) {
  return static_cast<void*>(static_cast<char*>(ptr) + offset);
}

template <typename Record> class SendBuffer {
public:
  explicit SendBuffer(size_t capacity_records) : capacity_records_(capacity_records) {}

  void release() {
    buffer_ = nullptr;
    record_count_ = 0;
  }

  bool empty() const {
    return record_count_ == 0;
  }

  size_t size() const {
    return record_count_;
  }

  size_t capacity() const {
    return capacity_records_;
  }

  void* data() const {
    return buffer_;
  }

  size_t size_in_bytes() const {
    return message_bytes<Record>(record_count_);
  }

  void finalize_header() {
    write_message_header<Record>(buffer_, record_count_);
  }

  void push(const Record& record, lci::device_t device, void* fallback_buffer) {
    if (buffer_ == nullptr) {
      buffer_ = fallback_buffer == nullptr ? allocate_upacket_blocking<Record>(device) : fallback_buffer;
    }
    void* dst =
        static_cast<void*>(static_cast<char*>(buffer_) + header_bytes<Record>() + record_count_ * sizeof(Record));
    std::memcpy(dst, &record, sizeof(Record));
    record_count_++;
  }

private:
  size_t capacity_records_;
  void* buffer_ = nullptr;
  size_t record_count_ = 0;
};

template <typename Record>
void post_send_buffer(int dest_rank, SendBuffer<Record>& send_buffer, int my_rank, lci::device_t device,
                      bool use_loopback, bool use_upacket, lci::comp_t send_counter, lci::rcomp_t remote_completion,
                      AmExchangeStateBase& state, std::atomic<size_t>& posted_send_count, size_t worker_index) {
  if (send_buffer.empty()) {
    return;
  }
  send_buffer.finalize_header();

  if (dest_rank == my_rank && use_loopback) {
    state.receive_message(my_rank, send_buffer.data(), send_buffer.size_in_bytes(), true, worker_index);
    if (use_upacket) {
      lci::put_upacket(send_buffer.data());
    }
    send_buffer.release();
    return;
  }

  lci::status_t status;
  do {
    status = lci::post_am_x(dest_rank, send_buffer.data(), send_buffer.size_in_bytes(), send_counter, remote_completion)
                 .comp_semantic(lci::comp_semantic_t::network)
                 .device(device)();
    progress_device(device);
  } while (status.is_retry());

  if (status.is_posted()) {
    posted_send_count.fetch_add(1, std::memory_order_relaxed);
  } else if (use_upacket) {
    lci::put_upacket(send_buffer.data());
  }
  send_buffer.release();
}

template <typename Record> size_t choose_batch_records(AmOptions options) {
  size_t max_bcopy = lci::get_max_bcopy_size();
  size_t header = header_bytes<Record>();
  if (max_bcopy <= header) {
    throw std::invalid_argument("LCI eager AM size cannot fit the irregular AM header");
  }

  size_t max_records = (max_bcopy - header) / sizeof(Record);
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

class AmRuntimeStateBase : public AmExchangeStateBase {
public:
  ~AmRuntimeStateBase() override = default;

  virtual void flush_worker(size_t worker_index) = 0;
  virtual void quiet_worker(size_t worker_index) = 0;
  virtual bool has_buffered_records() const = 0;
  virtual bool local_sends_drained() const noexcept = 0;
  virtual size_t received_record_count() const noexcept = 0;
  virtual AmProfile profile() const = 0;
  virtual void finalize_profile(IrregularRuntime& runtime) = 0;
};

template <typename Record> class TypedAmRuntimeStateBase : public AmRuntimeStateBase {
public:
  virtual void post_record(size_t worker_index, int dest_rank, const Record& record) = 0;
};

template <typename Record, typename AmHandler> class TypedAmRuntimeState : public TypedAmRuntimeStateBase<Record> {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(std::is_trivially_default_constructible<Record>::value,
                "Record must be trivially default constructible under C++17");
  static_assert(alignof(Record) <= alignof(std::max_align_t),
                "Record alignment greater than max_align_t is not supported");

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

public:
  TypedAmRuntimeState(IrregularRuntime& runtime, AmHandler am_handler, AmOptions options)
      : runtime_(&runtime), am_handler_(std::move(am_handler)),
        profile_(runtime.profiling_enabled(), runtime.profiling_worker_count(), 0, options.profile_name),
        options_(std::move(options)), batch_records_(choose_batch_records<Record>(options_)),
        bytes_per_buffer_(message_bytes<Record>(batch_records_)) {
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

  void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback,
                       std::optional<size_t> worker_index) noexcept override {
    try {
      receive_message_impl(source_rank, message, message_bytes, loopback, worker_index);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "LCI irregular receive callback terminated after exception: %s\n", error.what());
      std::terminate();
    } catch (...) {
      std::fprintf(stderr, "LCI irregular receive callback terminated after unknown exception\n");
      std::terminate();
    }
  }

  void post_record(size_t worker_index, int dest_rank, const Record& record) override {
    if (dest_rank < 0 || dest_rank >= runtime_->rank_count()) {
      throw std::invalid_argument("LCI irregular AM destination rank is out of range");
    }

    WorkerBuffers& buffers = worker_buffers(worker_index);
    void* fallback_buffer = nullptr;
    if (!options_.use_upacket) {
      fallback_buffer = byte_offset(buffers.fallback_storage, static_cast<size_t>(dest_rank) * bytes_per_buffer_);
    }

    SendBuffer<Record>& send_buffer = buffers.send_buffers[static_cast<size_t>(dest_rank)];
    ProgressWorkerScope worker_scope(profile_.enabled(), worker_index);
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

  void receive_message_impl(int source_rank, const void* message, size_t message_bytes, bool loopback,
                            std::optional<size_t> worker_index) {
    AmMessageHeader header{};
    std::memcpy(&header, message, sizeof(header));

    size_t expected_bytes = detail::message_bytes<Record>(header.record_count);
    if (message_bytes != expected_bytes) {
      std::fprintf(stderr, "LCI irregular AM payload size mismatch: got %zu expected %zu\n", message_bytes,
                   expected_bytes);
      std::abort();
    }

    const char* bytes = static_cast<const char*>(message);
    const void* payload = static_cast<const void*>(bytes + header_bytes<Record>());
    const size_t record_count = header.record_count;

    profile_.measure(loopback ? ProfileOperation::loopback_receive : ProfileOperation::remote_receive, worker_index,
                     record_count, record_count * sizeof(Record),
                     [&] { am_handler_(RecordBatchView<Record>(payload, record_count), source_rank); });
    received_record_count_.fetch_add(record_count, std::memory_order_relaxed);
  }

  WorkerBuffers& worker_buffers(size_t worker_index) {
    std::lock_guard<std::mutex> lock(worker_buffers_mutex_);
    if (worker_index >= worker_buffers_.size()) {
      worker_buffers_.resize(worker_index + 1);
    }
    std::unique_ptr<WorkerBuffers>& buffers = worker_buffers_[worker_index];
    if (!buffers) {
      buffers = std::make_unique<WorkerBuffers>(batch_records_, runtime_->rank_count(), bytes_per_buffer_,
                                                options_.use_upacket);
    }
    return *buffers;
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
      ProgressWorkerScope worker_scope(profile_.enabled(), worker_index);
      post_send_buffer(dest_rank, send_buffer, runtime_->rank(), device_for_worker(worker_index), options_.use_loopback,
                       options_.use_upacket, send_counter_, remote_completion(*runtime_), *this, posted_am_count_,
                       worker_index);
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
};

} // namespace detail

template <typename Record, typename AmHandler>
void IrregularRuntime::set_am_handler(AmHandler am_handler, AmOptions options) {
  clear_am_handler();
  auto state = std::make_unique<detail::TypedAmRuntimeState<Record, AmHandler>>(*this, std::move(am_handler),
                                                                                std::move(options));
  active_am_state_ = std::move(state);
  barrier();
}

template <typename Record> void IrregularRuntime::post_am(size_t worker_index, int dest_rank, const Record& record) {
  validate_worker_index(worker_index);
  auto* state = dynamic_cast<detail::TypedAmRuntimeStateBase<Record>*>(&active_am_state());
  if (state == nullptr) {
    throw std::logic_error("LCI irregular AM record type does not match the active handler");
  }
  state->post_record(worker_index, dest_rank, record);
}

} // namespace lci_irregular
