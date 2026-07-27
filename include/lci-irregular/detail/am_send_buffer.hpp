#pragma once

#include <lci-irregular/detail/am_dispatch.hpp>
#include <lci-irregular/detail/am_message.hpp>

#include <lci.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>

namespace lci_irregular {
namespace detail {

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
                      AmStateBase& state, std::atomic<size_t>& posted_send_count, size_t worker_index) {
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

} // namespace detail
} // namespace lci_irregular
