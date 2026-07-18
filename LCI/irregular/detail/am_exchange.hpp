#pragma once

#include "irregular/detail/am_exchange_state.hpp"

#include <lci.hpp>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace lci_irregular {
namespace detail {

inline size_t align_up(size_t value, size_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

template <typename Record>
size_t header_bytes() {
  return align_up(sizeof(AmMessageHeader), alignof(Record));
}

template <typename Record>
size_t message_bytes(size_t batch_records) {
  return header_bytes<Record>() + batch_records * sizeof(Record);
}

template <typename Record, typename ReceiveBatch>
class TypedAmExchangeState : public AmExchangeStateBase {
public:
  explicit TypedAmExchangeState(ReceiveBatch& receive_batch) : receive_batch_(receive_batch) {}

  void set_exchange_id(uint32_t exchange_id) {
    exchange_id_ = exchange_id;
  }

  void receive_message(int source_rank, const void* message, size_t message_bytes) override {
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
    if (reinterpret_cast<uintptr_t>(payload) % alignof(Record) == 0) {
      const Record* records = static_cast<const Record*>(payload);
      receive_batch_(records, header.record_count, source_rank);
      return;
    }

    void* aligned_payload = std::malloc(header.record_count * sizeof(Record));
    if (aligned_payload == nullptr && header.record_count != 0) {
      std::fprintf(stderr, "Failed to allocate aligned LCI irregular AM receive buffer\n");
      std::abort();
    }
    std::memcpy(aligned_payload, payload, header.record_count * sizeof(Record));
    const Record* records = static_cast<const Record*>(aligned_payload);
    receive_batch_(records, header.record_count, source_rank);
    std::free(aligned_payload);
  }

private:
  uint32_t exchange_id_ = 0;
  ReceiveBatch& receive_batch_;
};

template <typename Record>
void write_message_header(void* buffer, uint32_t exchange_id, size_t record_count) {
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

inline int openmp_thread_num() {
#ifdef _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

template <typename Record>
void* allocate_upacket_blocking(lci::device_t device) {
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

template <typename Record>
class SendBuffer {
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
                      AmExchangeStateBase& state, std::atomic<size_t>& posted_send_count) {
  if (send_buffer.empty()) {
    return;
  }

  if (dest_rank == my_rank && use_loopback) {
    state.receive_message(my_rank, send_buffer.data(), send_buffer.size_in_bytes());
    if (use_upacket) {
      lci::put_upacket(send_buffer.data());
    }
    send_buffer.release();
    return;
  }

  lci::status_t status;
  do {
    status = lci::post_am_x(dest_rank, send_buffer.data(), send_buffer.size_in_bytes(), send_counter,
                            remote_completion)
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

template <typename Record>
size_t choose_batch_records(AmExchangeOptions options) {
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

template <typename Record, typename RouteRecord, typename ReceiveBatch, typename IsDone>
void run_am_exchange(IrregularRuntime& runtime, const Record* records, size_t count, RouteRecord route_record,
                     ReceiveBatch receive_batch, IsDone is_done, AmExchangeOptions options) {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(alignof(Record) <= alignof(std::max_align_t),
                "Record alignment greater than max_align_t is not supported");
  if (records == nullptr && count != 0) {
    std::fprintf(stderr, "LCI irregular AM exchange received null records with nonzero count\n");
    std::abort();
  }

  size_t batch_records = choose_batch_records<Record>(options);
  TypedAmExchangeState<Record, ReceiveBatch> state(receive_batch);
  uint32_t exchange_id = register_exchange(runtime, &state);
  state.set_exchange_id(exchange_id);

  lci::counter_set(send_counter(runtime), 0);
  std::atomic<size_t> posted_send_count{0};

  const auto& runtime_devices = devices(runtime);
  const int comm_size = runtime.rank_count();
  const int my_rank = runtime.rank();
  const size_t bytes_per_buffer = message_bytes<Record>(batch_records);
  const int max_threads = runtime.max_threads();
  void* fallback_storage = nullptr;
  if (!options.use_upacket) {
    fallback_storage =
        std::malloc(bytes_per_buffer * static_cast<size_t>(comm_size) * static_cast<size_t>(max_threads));
    if (fallback_storage == nullptr) {
      std::fprintf(stderr, "Failed to allocate LCI irregular AM fallback buffers\n");
      std::abort();
    }
  }

  #pragma omp parallel
  {
    const int thread_id = openmp_thread_num();
    lci::device_t device = runtime_devices[static_cast<size_t>(thread_id) % runtime_devices.size()];
    std::vector<SendBuffer<Record>> send_buffers;
    send_buffers.reserve(comm_size);
    for (int rank = 0; rank < comm_size; ++rank) {
      send_buffers.emplace_back(exchange_id, batch_records);
    }

    #pragma omp for nowait
    for (size_t i = 0; i < count; ++i) {
      int dest_rank = route_record(records[i]);
      if (dest_rank < 0 || dest_rank >= comm_size) {
        std::fprintf(stderr, "LCI irregular AM route returned invalid rank %d on rank %d\n", dest_rank, my_rank);
        std::abort();
      }

      void* fallback_buffer = nullptr;
      if (!options.use_upacket) {
        size_t offset =
            (static_cast<size_t>(thread_id) * static_cast<size_t>(comm_size) + static_cast<size_t>(dest_rank)) *
            bytes_per_buffer;
        fallback_buffer = byte_offset(fallback_storage, offset);
      }

      SendBuffer<Record>& send_buffer = send_buffers[dest_rank];
      send_buffer.push(records[i], device, fallback_buffer);
      if (send_buffer.size() == send_buffer.capacity()) {
        post_send_buffer(dest_rank, send_buffer, my_rank, device, options.use_loopback, options.use_upacket,
                         send_counter(runtime), remote_completion(runtime), state, posted_send_count);
      }
    }

    for (int i = 0; i < comm_size; ++i) {
      int dest_rank = (my_rank + i) % comm_size;
      post_send_buffer(dest_rank, send_buffers[dest_rank], my_rank, device, options.use_loopback, options.use_upacket,
                       send_counter(runtime), remote_completion(runtime), state, posted_send_count);
    }

    while (!is_done() || static_cast<size_t>(lci::counter_get(send_counter(runtime))) <
                             posted_send_count.load(std::memory_order_relaxed)) {
      progress_device(device);
    }
  }

  if (fallback_storage != nullptr) {
    std::free(fallback_storage);
  }
  deregister_exchange(runtime, exchange_id);
}

} // namespace detail

template <typename Record, typename RouteRecord, typename ReceiveBatch>
void IrregularRuntime::am_exchange_counted(const Record* records, size_t count, RouteRecord route_record,
                                           ReceiveBatch receive_batch, size_t expected_recv_count,
                                           AmExchangeOptions options) {
  std::atomic<size_t> received_count{0};
  auto counted_receive = [&](const Record* batch, size_t batch_count, int source_rank) {
    receive_batch(batch, batch_count, source_rank);
    received_count.fetch_add(batch_count, std::memory_order_relaxed);
  };
  auto is_done = [&]() {
    return received_count.load(std::memory_order_relaxed) >= expected_recv_count;
  };
  am_exchange_until<Record>(records, count, route_record, counted_receive, is_done, options);
}

template <typename Record, typename RouteRecord, typename ReceiveBatch, typename IsDone>
void IrregularRuntime::am_exchange_until(const Record* records, size_t count, RouteRecord route_record,
                                         ReceiveBatch receive_batch, IsDone is_done, AmExchangeOptions options) {
  detail::run_am_exchange<Record>(*this, records, count, route_record, receive_batch, is_done, options);
}

} // namespace lci_irregular
