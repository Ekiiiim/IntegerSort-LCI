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
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lci_irregular {
namespace detail {

inline size_t align_up(size_t value, size_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

template <typename Record> size_t header_bytes() {
  return align_up(sizeof(AmMessageHeader), alignof(Record));
}

template <typename Record> size_t message_bytes(size_t batch_records) {
  return header_bytes<Record>() + batch_records * sizeof(Record);
}

template <typename Record, typename ReceiveBatch> class TypedAmExchangeState : public AmExchangeStateBase {
public:
  TypedAmExchangeState(ReceiveBatch& receive_batch, AmProfileRecorder& profile)
      : receive_batch_(receive_batch), profile_(profile) {}

  void set_exchange_id(uint32_t exchange_id) {
    exchange_id_ = exchange_id;
  }

  void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback,
                       std::optional<size_t> worker_index) override {
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
    void* aligned_payload = nullptr;
    const Record* records = static_cast<const Record*>(payload);
    if (reinterpret_cast<uintptr_t>(payload) % alignof(Record) != 0) {
      aligned_payload = std::malloc(header.record_count * sizeof(Record));
      if (aligned_payload == nullptr && header.record_count != 0) {
        throw std::bad_alloc();
      }
      std::memcpy(aligned_payload, payload, header.record_count * sizeof(Record));
      records = static_cast<const Record*>(aligned_payload);
    }

    const auto start = std::chrono::steady_clock::now();
    receive_batch_(records, header.record_count, source_rank);
    std::free(aligned_payload);
    profile_.record(loopback ? ProfileOperation::loopback_receive : ProfileOperation::remote_receive, worker_index,
                    header.record_count, header.record_count * sizeof(Record), elapsed_nanoseconds(start));
  }

private:
  uint32_t exchange_id_ = 0;
  ReceiveBatch& receive_batch_;
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

  void push(const Record& record, lci::device_t device, void* fallback_buffer) {
    if (buffer_ == nullptr) {
      buffer_ = fallback_buffer == nullptr ? allocate_upacket_blocking<Record>(device) : fallback_buffer;
      write_message_header<Record>(buffer_, exchange_id_, 0);
    }
    void* dst =
        static_cast<void*>(static_cast<char*>(buffer_) + header_bytes<Record>() + record_count_ * sizeof(Record));
    std::memcpy(dst, &record, sizeof(Record));
    record_count_++;
    write_message_header<Record>(buffer_, exchange_id_, record_count_);
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
  size_t lci_eager_margin = sizeof(lci::tag_t) + sizeof(lci::rcomp_t);
  if (max_bcopy <= header + lci_eager_margin) {
    std::fprintf(stderr, "LCI eager AM size %zu cannot fit irregular AM header %zu with LCI metadata margin %zu\n",
                 max_bcopy, header, lci_eager_margin);
    std::abort();
  }

  size_t max_records = (max_bcopy - header - lci_eager_margin) / sizeof(Record);
  if (max_records == 0) {
    std::fprintf(stderr, "LCI eager AM size %zu cannot fit one irregular AM record of %zu bytes\n", max_bcopy,
                 sizeof(Record));
    std::abort();
  }
  if (options.batch_records == 0) {
    return max_records;
  }
  if (options.batch_records > max_records) {
    std::fprintf(stderr, "Requested AM batch size %zu exceeds maximum %zu for this record type\n",
                 options.batch_records, max_records);
    std::abort();
  }
  return options.batch_records;
}

} // namespace detail

template <typename Record, typename ReceiveBatch, typename IsDone> class AmExchange {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(alignof(Record) <= alignof(std::max_align_t),
                "Record alignment greater than max_align_t is not supported");

public:
  class Sender {
  public:
    Sender(AmExchange& exchange, size_t worker_index)
        : exchange_(&exchange), worker_index_(worker_index), send_buffers_(make_send_buffers(exchange)) {
      if (!exchange_->options_.use_upacket) {
        size_t storage_bytes = exchange_->bytes_per_buffer_ * static_cast<size_t>(exchange_->rank_count());
        fallback_storage_ = std::malloc(storage_bytes);
        if (fallback_storage_ == nullptr && storage_bytes != 0) {
          std::fprintf(stderr, "Failed to allocate LCI irregular AM fallback buffers\n");
          std::abort();
        }
      }
    }

    ~Sender() {
      cleanup();
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    Sender(Sender&& other) noexcept
        : exchange_(other.exchange_), worker_index_(other.worker_index_), send_buffers_(std::move(other.send_buffers_)),
          fallback_storage_(other.fallback_storage_) {
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
      send_buffers_ = std::move(other.send_buffers_);
      fallback_storage_ = other.fallback_storage_;
      other.exchange_ = nullptr;
      other.fallback_storage_ = nullptr;
      return *this;
    }

    void am_send(int dest_rank, const Record& record) {
      validate_active();
      if (dest_rank < 0 || dest_rank >= exchange_->rank_count()) {
        std::fprintf(stderr, "LCI irregular AM route returned invalid rank %d on rank %d\n", dest_rank,
                     exchange_->rank());
        std::abort();
      }

      void* fallback_buffer = nullptr;
      if (!exchange_->options_.use_upacket) {
        fallback_buffer =
            detail::byte_offset(fallback_storage_, static_cast<size_t>(dest_rank) * exchange_->bytes_per_buffer_);
      }

      detail::SendBuffer<Record>& send_buffer = send_buffers_[static_cast<size_t>(dest_rank)];
      detail::ProgressWorkerScope worker_scope(worker_index_);
      send_buffer.push(record, exchange_->device_for_worker(worker_index_), fallback_buffer);
      if (send_buffer.size() == send_buffer.capacity()) {
        flush(dest_rank);
      }
    }

    void flush() {
      validate_active();
      int my_rank = exchange_->rank();
      int comm_size = exchange_->rank_count();
      for (int i = 0; i < comm_size; ++i) {
        flush((my_rank + i) % comm_size);
      }
    }

  private:
    static std::vector<detail::SendBuffer<Record>> make_send_buffers(AmExchange& exchange) {
      std::vector<detail::SendBuffer<Record>> send_buffers;
      send_buffers.reserve(static_cast<size_t>(exchange.rank_count()));
      for (int rank = 0; rank < exchange.rank_count(); ++rank) {
        send_buffers.emplace_back(exchange.exchange_id_, exchange.batch_records_);
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
      const auto start = std::chrono::steady_clock::now();
      detail::ProgressWorkerScope worker_scope(worker_index_);
      detail::post_send_buffer(dest_rank, send_buffers_[static_cast<size_t>(dest_rank)], exchange_->rank(),
                               exchange_->device_for_worker(worker_index_), exchange_->options_.use_loopback,
                               exchange_->options_.use_upacket, exchange_->send_counter_,
                               detail::remote_completion(*exchange_->runtime_), exchange_->state_,
                               exchange_->posted_send_count_, worker_index_);
      exchange_->profile_.record(detail::ProfileOperation::flush, worker_index_, record_count, payload_bytes,
                                 detail::elapsed_nanoseconds(start));
    }

    void validate_active() const {
      if (exchange_ == nullptr) {
        std::fprintf(stderr, "LCI irregular AM sender used after move\n");
        std::abort();
      }
    }

    void cleanup() {
      if (exchange_ != nullptr) {
        flush();
      }
      std::free(fallback_storage_);
      fallback_storage_ = nullptr;
      exchange_ = nullptr;
    }

    AmExchange* exchange_ = nullptr;
    size_t worker_index_ = 0;
    std::vector<detail::SendBuffer<Record>> send_buffers_;
    void* fallback_storage_ = nullptr;
  };

  AmExchange(IrregularRuntime& runtime, ReceiveBatch receive_batch, IsDone is_done, AmExchangeOptions options)
      : runtime_(&runtime), receive_batch_(std::move(receive_batch)), is_done_(std::move(is_done)),
        profile_(runtime.profiling_enabled(), runtime.profiling_worker_count(), 0, options.profile_name),
        state_(receive_batch_, profile_), options_(options),
        batch_records_(detail::choose_batch_records<Record>(options)),
        bytes_per_buffer_(detail::message_bytes<Record>(batch_records_)), send_counter_(lci::alloc_counter()) {
    lci::counter_set(send_counter_, 0);
    exchange_id_ = detail::register_exchange(runtime, &state_);
    state_.set_exchange_id(exchange_id_);
    profile_.set_exchange_sequence(exchange_id_);
    registered_ = true;
  }

  ~AmExchange() {
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
    const auto start = std::chrono::steady_clock::now();
    runtime_->progress();
    profile_.record(detail::ProfileOperation::progress, std::nullopt, 0, 0, detail::elapsed_nanoseconds(start));
  }

  void progress(size_t worker_index) {
    validate_profile_worker(worker_index);
    const auto start = std::chrono::steady_clock::now();
    runtime_->progress(worker_index);
    profile_.record(detail::ProfileOperation::progress, worker_index, 0, 0, detail::elapsed_nanoseconds(start));
  }

  bool is_done() {
    return is_done_() && local_sends_complete();
  }

  void wait() {
    while (!is_done()) {
      progress();
    }
    finalize_profile_once();
  }

  AmExchangeProfile profile() {
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
    return is_done_() && local_sends_complete();
  }

  void validate_profile_worker(size_t worker_index) const {
    if (runtime_->profiling_enabled() && worker_index >= runtime_->profiling_worker_count()) {
      throw std::invalid_argument("LCI irregular AM worker index exceeds profiling worker_count");
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
  ReceiveBatch receive_batch_;
  IsDone is_done_;
  detail::AmProfileRecorder profile_;
  detail::TypedAmExchangeState<Record, ReceiveBatch> state_;
  AmExchangeOptions options_;
  size_t batch_records_ = 0;
  size_t bytes_per_buffer_ = 0;
  lci::comp_t send_counter_ = nullptr;
  std::atomic<size_t> posted_send_count_{0};
  uint32_t exchange_id_ = 0;
  bool registered_ = false;
  std::once_flag profile_finalize_once_;
};

template <typename Record, typename ReceiveBatch, typename IsDone>
AmExchange<Record, ReceiveBatch, IsDone> IrregularRuntime::am_exchange_start(ReceiveBatch receive_batch, IsDone is_done,
                                                                             AmExchangeOptions options) {
  return AmExchange<Record, ReceiveBatch, IsDone>(*this, std::move(receive_batch), std::move(is_done), options);
}

template <typename Record, typename RouteRecord, typename ReceiveBatch>
AmExchangeProfile IrregularRuntime::am_exchange_counted(const Record* records, size_t count, RouteRecord route_record,
                                                        ReceiveBatch receive_batch, size_t expected_recv_count,
                                                        AmExchangeOptions options) {
  std::atomic<size_t> received_count{0};
  auto counted_receive = [&](const Record* batch, size_t batch_count, int source_rank) {
    receive_batch(batch, batch_count, source_rank);
    received_count.fetch_add(batch_count, std::memory_order_relaxed);
  };
  auto is_done = [&]() { return received_count.load(std::memory_order_relaxed) >= expected_recv_count; };
  return am_exchange_until<Record>(records, count, route_record, counted_receive, is_done, options);
}

template <typename Record, typename RouteRecord, typename ReceiveBatch, typename IsDone>
AmExchangeProfile IrregularRuntime::am_exchange_until(const Record* records, size_t count, RouteRecord route_record,
                                                      ReceiveBatch receive_batch, IsDone is_done,
                                                      AmExchangeOptions options) {
  if (records == nullptr && count != 0) {
    std::fprintf(stderr, "LCI irregular AM exchange received null records with nonzero count\n");
    std::abort();
  }

  auto exchange = am_exchange_start<Record>(std::move(receive_batch), std::move(is_done), options);
  auto sender = exchange.make_sender();
  for (size_t i = 0; i < count; ++i) {
    sender.am_send(route_record(records[i]), records[i]);
  }
  sender.flush();
  exchange.wait();
  return exchange.profile();
}

} // namespace lci_irregular
