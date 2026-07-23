#pragma once

#include <lci-irregular/detail/am_exchange_state.hpp>
#include <lci-irregular/detail/profiling.hpp>

#include <lci.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
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

template <typename Record, typename AmHandler> class TypedAmExchangeState : public AmExchangeStateBase {
public:
  TypedAmExchangeState(AmHandler& am_handler, AmProfileRecorder& profile)
      : am_handler_(am_handler), profile_(profile) {}

  void set_exchange_id(uint32_t exchange_id) {
    exchange_id_ = exchange_id;
  }

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

private:
  void receive_message_impl(int source_rank, const void* message, size_t message_bytes, bool loopback,
                            std::optional<size_t> worker_index) {
    AmMessageHeader header{};
    std::memcpy(&header, message, sizeof(header));
    if (header.exchange_id != exchange_id_) {
      std::fprintf(stderr, "LCI irregular AM exchange id mismatch: got %u expected %u\n", header.exchange_id,
                   exchange_id_);
      std::abort();
    }

    size_t expected_bytes = detail::message_bytes<Record>(header.record_count);
    if (message_bytes != expected_bytes) {
      std::fprintf(stderr, "LCI irregular AM payload size mismatch: got %zu expected %zu\n", message_bytes,
                   expected_bytes);
      std::abort();
    }

    const char* bytes = static_cast<const char*>(message);
    const void* payload = static_cast<const void*>(bytes + header_bytes<Record>());

    profile_.measure(loopback ? ProfileOperation::loopback_receive : ProfileOperation::remote_receive, worker_index,
                     header.record_count, header.record_count * sizeof(Record),
                     [&] { am_handler_(RecordBatchView<Record>(payload, header.record_count), source_rank); });
  }

  uint32_t exchange_id_ = 0;
  AmHandler& am_handler_;
  AmProfileRecorder& profile_;
};

template <typename Record> void write_message_header(void* buffer, uint32_t exchange_id, size_t record_count) {
  if (record_count > std::numeric_limits<uint32_t>::max()) {
    std::fprintf(stderr, "LCI irregular AM batch has too many records: %zu\n", record_count);
    std::abort();
  }

  AmMessageHeader header{exchange_id, static_cast<uint32_t>(record_count)};
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
  SendBuffer(uint32_t exchange_id, size_t capacity_records)
      : exchange_id_(exchange_id), capacity_records_(capacity_records) {}

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
    write_message_header<Record>(buffer_, exchange_id_, record_count_);
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
  uint32_t exchange_id_;
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

template <typename Record> size_t choose_batch_records(AmExchangeOptions options) {
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

} // namespace detail

template <typename Record, typename AmHandler, typename IsDone> class AmExchange {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(std::is_trivially_default_constructible<Record>::value,
                "Record must be trivially default constructible under C++17");
  static_assert(alignof(Record) <= alignof(std::max_align_t),
                "Record alignment greater than max_align_t is not supported");

public:
  class Sender {
  public:
    Sender(AmExchange& exchange, size_t worker_index)
        : exchange_(&exchange), worker_index_(worker_index), rank_count_(exchange.rank_count()), rank_(exchange.rank()),
          device_(exchange.device_for_worker(worker_index)), profiling_enabled_(exchange.profile_.enabled()),
          use_upacket_(exchange.options_.use_upacket), use_loopback_(exchange.options_.use_loopback),
          bytes_per_buffer_(exchange.bytes_per_buffer_),
          remote_completion_(detail::remote_completion(*exchange.runtime_)),
          send_buffers_(make_send_buffers(exchange.exchange_id_, exchange.batch_records_, rank_count_)) {
      if (!use_upacket_) {
        size_t storage_bytes = bytes_per_buffer_ * static_cast<size_t>(rank_count_);
        fallback_storage_ = std::malloc(storage_bytes);
        if (fallback_storage_ == nullptr && storage_bytes != 0) {
          throw std::bad_alloc();
        }
      }
      exchange_->active_sender_count_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Sender() noexcept {
      if (exchange_ == nullptr) {
        return;
      }
      try {
        close();
      } catch (const std::exception& error) {
        std::fprintf(stderr, "LCI irregular AM sender cleanup terminated after exception: %s\n", error.what());
        std::terminate();
      } catch (...) {
        std::fprintf(stderr, "LCI irregular AM sender cleanup terminated after unknown exception\n");
        std::terminate();
      }
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    Sender(Sender&& other) noexcept
        : exchange_(other.exchange_), worker_index_(other.worker_index_), rank_count_(other.rank_count_),
          rank_(other.rank_), device_(other.device_), profiling_enabled_(other.profiling_enabled_),
          use_upacket_(other.use_upacket_), use_loopback_(other.use_loopback_),
          bytes_per_buffer_(other.bytes_per_buffer_), remote_completion_(other.remote_completion_),
          send_buffers_(std::move(other.send_buffers_)), fallback_storage_(other.fallback_storage_) {
      other.exchange_ = nullptr;
      other.fallback_storage_ = nullptr;
    }

    Sender& operator=(Sender&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      cleanup();
      exchange_ = other.exchange_;
      worker_index_ = other.worker_index_;
      rank_count_ = other.rank_count_;
      rank_ = other.rank_;
      device_ = other.device_;
      profiling_enabled_ = other.profiling_enabled_;
      use_upacket_ = other.use_upacket_;
      use_loopback_ = other.use_loopback_;
      bytes_per_buffer_ = other.bytes_per_buffer_;
      remote_completion_ = other.remote_completion_;
      send_buffers_ = std::move(other.send_buffers_);
      fallback_storage_ = other.fallback_storage_;
      other.exchange_ = nullptr;
      other.fallback_storage_ = nullptr;
      return *this;
    }

    // Flush buffered records and release this sender from its exchange.
    void close() {
      validate_active();
      try {
        flush();
      } catch (...) {
        release();
        throw;
      }
      release();
    }

    void am_send(int dest_rank, const Record& record) {
      validate_active();
      if (dest_rank < 0 || dest_rank >= rank_count_) {
        throw std::invalid_argument("LCI irregular AM route returned an invalid destination rank");
      }

      void* fallback_buffer = nullptr;
      if (!use_upacket_) {
        fallback_buffer = detail::byte_offset(fallback_storage_, static_cast<size_t>(dest_rank) * bytes_per_buffer_);
      }

      detail::SendBuffer<Record>& send_buffer = send_buffers_[static_cast<size_t>(dest_rank)];
      detail::ProgressWorkerScope worker_scope(profiling_enabled_, worker_index_);
      send_buffer.push(record, device_, fallback_buffer);
      if (send_buffer.size() == send_buffer.capacity()) {
        flush(dest_rank);
      }
    }

    void flush() {
      validate_active();
      for (int i = 0; i < rank_count_; ++i) {
        flush((rank_ + i) % rank_count_);
      }
    }

  private:
    static std::vector<detail::SendBuffer<Record>> make_send_buffers(uint32_t exchange_id, size_t batch_records,
                                                                     int rank_count) {
      std::vector<detail::SendBuffer<Record>> send_buffers;
      send_buffers.reserve(static_cast<size_t>(rank_count));
      for (int rank = 0; rank < rank_count; ++rank) {
        send_buffers.emplace_back(exchange_id, batch_records);
      }
      return send_buffers;
    }

    void flush(int dest_rank) {
      detail::SendBuffer<Record>& send_buffer = send_buffers_[static_cast<size_t>(dest_rank)];
      if (send_buffer.empty()) {
        return;
      }

      const size_t record_count = send_buffer.size();
      const size_t payload_bytes = record_count * sizeof(Record);
      exchange_->profile_.measure(detail::ProfileOperation::flush, worker_index_, record_count, payload_bytes, [&] {
        detail::ProgressWorkerScope worker_scope(profiling_enabled_, worker_index_);
        detail::post_send_buffer(dest_rank, send_buffers_[static_cast<size_t>(dest_rank)], rank_, device_,
                                 use_loopback_, use_upacket_, exchange_->send_counter_, remote_completion_,
                                 exchange_->state_, exchange_->posted_send_count_, worker_index_);
      });
    }

    void validate_active() const {
      if (exchange_ == nullptr) {
        throw std::logic_error("LCI irregular AM sender used after move");
      }
    }

    void release() noexcept {
      if (exchange_ != nullptr) {
        exchange_->active_sender_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      std::free(fallback_storage_);
      fallback_storage_ = nullptr;
      exchange_ = nullptr;
    }

    void cleanup() noexcept {
      if (exchange_ == nullptr) {
        return;
      }
      try {
        close();
      } catch (...) {
        std::terminate();
      }
    }

    AmExchange* exchange_ = nullptr;
    size_t worker_index_ = 0;
    int rank_count_ = 0;
    int rank_ = 0;
    lci::device_t device_;
    bool profiling_enabled_ = false;
    bool use_upacket_ = true;
    bool use_loopback_ = true;
    size_t bytes_per_buffer_ = 0;
    lci::rcomp_t remote_completion_ = 0;
    std::vector<detail::SendBuffer<Record>> send_buffers_;
    void* fallback_storage_ = nullptr;
  };

  AmExchange(IrregularRuntime& runtime, AmHandler am_handler, IsDone is_done, AmExchangeOptions options)
      : runtime_(&runtime), am_handler_(std::move(am_handler)), is_done_(std::move(is_done)),
        profile_(runtime.profiling_enabled(), runtime.profiling_worker_count(), 0, options.profile_name),
        state_(am_handler_, profile_), options_(options), batch_records_(detail::choose_batch_records<Record>(options)),
        bytes_per_buffer_(detail::message_bytes<Record>(batch_records_)) {
    send_counter_ = lci::alloc_counter();
    try {
      lci::counter_set(send_counter_, 0);
      exchange_id_ = detail::register_exchange(runtime, &state_);
      state_.set_exchange_id(exchange_id_);
      profile_.set_exchange_sequence(exchange_id_);
      registered_ = true;
    } catch (...) {
      lci::free_comp(&send_counter_);
      throw;
    }
  }

  ~AmExchange() {
    if (active_sender_count_.load(std::memory_order_relaxed) != 0) {
      std::fprintf(stderr, "LCI irregular AM exchange destroyed with active senders\n");
      std::terminate();
    }
    if (!is_complete_without_progress()) {
      std::fprintf(stderr, "LCI irregular AM exchange destroyed before completion\n");
      std::terminate();
    }
    finalize_profile_once();
    if (registered_) {
      detail::deregister_exchange(*runtime_, exchange_id_);
    }
    if (!send_counter_.is_empty()) {
      lci::free_comp(&send_counter_);
    }
  }

  AmExchange(const AmExchange&) = delete;
  AmExchange& operator=(const AmExchange&) = delete;
  AmExchange(AmExchange&&) = delete;
  AmExchange& operator=(AmExchange&&) = delete;

  Sender make_sender(size_t worker_index = 0) {
    validate_profile_worker(worker_index);
    return Sender(*this, worker_index);
  }

  void progress() {
    profile_.measure(detail::ProfileOperation::progress, std::nullopt, 0, 0, [&] { runtime_->progress(); });
  }

  void progress(size_t worker_index) {
    validate_profile_worker(worker_index);
    profile_.measure(detail::ProfileOperation::progress, worker_index, 0, 0, [&] { runtime_->progress(worker_index); });
  }

  bool is_done() {
    return invoke_is_done() && local_sends_complete();
  }

  // No other sender or progress worker may operate on this exchange while
  // wait() or profile() finalizes its statistics and lifetime state.
  void wait() {
    validate_no_active_senders();
    while (!is_done()) {
      progress();
    }
    finalize_profile_once();
  }

  AmExchangeProfile profile() {
    validate_no_active_senders();
    if (!is_complete_without_progress()) {
      throw std::logic_error("LCI irregular AM profile requested before exchange completion");
    }
    finalize_profile_once();
    return profile_.snapshot();
  }

private:
  int rank() const {
    return runtime_->rank();
  }

  int rank_count() const {
    return runtime_->rank_count();
  }

  const lci::device_t& device_for_worker(size_t worker_index) const {
    const auto& runtime_devices = detail::devices(*runtime_);
    return runtime_devices[worker_index % runtime_devices.size()];
  }

  bool local_sends_complete() const {
    return static_cast<size_t>(lci::counter_get(send_counter_)) >= posted_send_count_.load(std::memory_order_relaxed);
  }

  bool is_complete_without_progress() {
    return invoke_is_done() && local_sends_complete();
  }

  bool invoke_is_done() noexcept {
    try {
      return is_done_();
    } catch (const std::exception& error) {
      std::fprintf(stderr, "LCI irregular completion callback terminated after exception: %s\n", error.what());
      std::terminate();
    } catch (...) {
      std::fprintf(stderr, "LCI irregular completion callback terminated after unknown exception\n");
      std::terminate();
    }
  }

  void validate_profile_worker(size_t worker_index) const {
    if (runtime_->profiling_enabled() && worker_index >= runtime_->profiling_worker_count()) {
      throw std::invalid_argument("LCI irregular AM worker index exceeds profiling worker_count");
    }
  }

  void validate_no_active_senders() const {
    if (active_sender_count_.load(std::memory_order_relaxed) != 0) {
      throw std::logic_error("LCI irregular AM senders must be closed before exchange finalization");
    }
  }

  void finalize_profile_once() {
    std::call_once(profile_finalize_once_, [this] {
      if (profile_.enabled()) {
        detail::submit_profile(*runtime_, profile_.snapshot());
      }
    });
  }

  IrregularRuntime* runtime_;
  AmHandler am_handler_;
  IsDone is_done_;
  detail::AmProfileRecorder profile_;
  detail::TypedAmExchangeState<Record, AmHandler> state_;
  AmExchangeOptions options_;
  size_t batch_records_ = 0;
  size_t bytes_per_buffer_ = 0;
  lci::comp_t send_counter_ = nullptr;
  std::atomic<size_t> posted_send_count_{0};
  std::atomic<size_t> active_sender_count_{0};
  uint32_t exchange_id_ = 0;
  bool registered_ = false;
  std::once_flag profile_finalize_once_;
};

template <typename Record, typename AmHandler, typename IsDone>
AmExchange<Record, AmHandler, IsDone> IrregularRuntime::am_exchange_start(AmHandler am_handler, IsDone is_done,
                                                                          AmExchangeOptions options) {
  return AmExchange<Record, AmHandler, IsDone>(*this, std::move(am_handler), std::move(is_done), options);
}

template <typename Record, typename AmHandler, typename IsDone, typename SendPhase>
AmExchangeProfile IrregularRuntime::am_exchange_until(AmHandler am_handler, IsDone is_done, SendPhase send_phase,
                                                      AmExchangeOptions options) {
  auto exchange = am_exchange_start<Record>(std::move(am_handler), std::move(is_done), options);
  barrier();

  std::atomic<size_t> worker_calls{0};
  std::atomic<size_t> active_worker_calls{0};
  auto run_worker = [&](size_t worker_index, auto&& produce) noexcept {
    worker_calls.fetch_add(1);
    active_worker_calls.fetch_add(1);
    try {
      auto sender = exchange.make_sender(worker_index);
      auto am_send = [&](int destination_rank, const Record& record) { sender.am_send(destination_rank, record); };
      std::forward<decltype(produce)>(produce)(am_send);
      sender.close();
      while (!exchange.is_done()) {
        exchange.progress(worker_index);
      }
      active_worker_calls.fetch_sub(1);
    } catch (...) {
      detail::terminate_current_exception("LCI irregular AM worker terminated");
    }
  };

  try {
    std::move(send_phase)(run_worker);
    if (worker_calls.load() == 0) {
      throw std::logic_error("LCI irregular AM send phase did not run a worker");
    }
    if (active_worker_calls.load() != 0) {
      throw std::logic_error("LCI irregular AM send phase returned with active workers");
    }
    exchange.wait();
    return exchange.profile();
  } catch (...) {
    detail::terminate_current_exception("LCI irregular AM send phase terminated");
  }
}

} // namespace lci_irregular
