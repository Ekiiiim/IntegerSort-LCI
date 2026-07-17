#include "communication/lci_redistributor.hpp"

#include "communication/profiling/a2a_thread_profile.hpp"

#include <omp.h>

#include <cstdlib>
#include <vector>

namespace is_lci {
namespace {

constexpr size_t KEY_SIZE = sizeof(KeyValue);
RedistributorRuntime* active_runtime = nullptr;
bool active_runtime_uses_upacket = true;

void handle_received_keys(const void* src, size_t num_keys) {
  const KeyValue* keys = static_cast<const KeyValue*>(src);
  for (size_t i = 0; i < num_keys; ++i) {
    active_runtime->frequency_histogram[keys[i]].fetch_add(1, std::memory_order_relaxed);
  }
  active_runtime->received_count.fetch_add(num_keys, std::memory_order_relaxed);
}

void am_handler(lci::status_t status) {
#ifdef A2A_TL_TIMERS
  TL_STEP_START(A2A_AM_COPY);
#endif
  handle_received_keys(status.get_buffer(), status.get_size() / KEY_SIZE);
#ifdef A2A_TL_TIMERS
  TL_STEP_STOP(A2A_AM_COPY);
  TL_ADD_BYTES(A2A_AM_COPY, status.get_size());
#endif
  if (active_runtime_uses_upacket) {
    lci::put_upacket(status.get_buffer());
  }
}

void* get_upacket_blocking(lci::device_t device, int dest_rank, int comm_size, const RedistributorOptions& options,
                           RedistributorRuntime* runtime) {
  if (options.use_upacket) {
    void* upacket = lci::get_upacket();
    while (upacket == nullptr) {
      upacket = lci::get_upacket();
      lci::progress_x().device(device)();
    }
    return upacket;
  }

  const int thread_id = omp_get_thread_num();
  const int buffer_index = (thread_id * comm_size + dest_rank) * options.message_batch_size;
  return static_cast<void*>(static_cast<KeyValue*>(runtime->fallback_buffer) + buffer_index);
}

class SendBuffer {
public:
  void release() {
    buffer_ = nullptr;
    values_ = nullptr;
    size_ = 0;
  }

  void push(KeyValue key, lci::device_t device, int dest_rank, int comm_size, const RedistributorOptions& options,
            RedistributorRuntime* runtime) {
    if (buffer_ == nullptr) {
      buffer_ = get_upacket_blocking(device, dest_rank, comm_size, options, runtime);
      values_ = static_cast<KeyValue*>(buffer_);
    }
    values_[size_++] = key;
  }

  void* data() const {
    return buffer_;
  }
  size_t size() const {
    return static_cast<size_t>(size_);
  }
  size_t size_in_bytes() const {
    return static_cast<size_t>(size_) * KEY_SIZE;
  }
  bool empty() const {
    return size_ == 0;
  }

private:
  void* buffer_ = nullptr;
  KeyValue* values_ = nullptr;
  int size_ = 0;
};

void flush_send_buffer(std::vector<SendBuffer>& send_buffers, int dest_rank, int my_rank, lci::device_t device,
                       const RedistributorOptions& options, RedistributorRuntime* runtime) {
  SendBuffer& send_buffer = send_buffers[dest_rank];
  if (send_buffer.empty()) {
    return;
  }

#ifdef A2A_TL_TIMERS
  TL_STEP_START(A2A_FLUSH_SEND);
#endif

  if (dest_rank == my_rank && options.use_loopback) {
#ifdef A2A_TL_TIMERS
    TL_STEP_START(A2A_SELF_COPY);
#endif
    handle_received_keys(send_buffer.data(), send_buffer.size());
    if (options.use_upacket) {
      lci::put_upacket(send_buffer.data());
    }
#ifdef A2A_TL_TIMERS
    TL_STEP_STOP(A2A_SELF_COPY);
    TL_ADD_BYTES(A2A_SELF_COPY, send_buffer.size_in_bytes());
#endif
  } else {
    lci::status_t status;
    do {
      status = lci::post_am_x(dest_rank, send_buffer.data(), send_buffer.size_in_bytes(), runtime->send_counter,
                              runtime->rcomp)
                   .comp_semantic(lci::comp_semantic_t::network)
                   .device(device)();
      lci::progress_x().device(device)();
    } while (status.is_retry());
  }

#ifdef A2A_TL_TIMERS
  TL_STEP_STOP(A2A_FLUSH_SEND);
  TL_ADD_BYTES(A2A_FLUSH_SEND, send_buffer.size_in_bytes());
#endif
  send_buffer.release();
}

void send_key_to_processor(KeyValue key, int dest_rank, std::vector<SendBuffer>& send_buffers, int comm_size,
                           int my_rank, lci::device_t device, const RedistributorOptions& options,
                           RedistributorRuntime* runtime) {
  send_buffers[dest_rank].push(key, device, dest_rank, comm_size, options, runtime);
  if (send_buffers[dest_rank].size() >= static_cast<size_t>(options.message_batch_size)) {
    flush_send_buffer(send_buffers, dest_rank, my_rank, device, options, runtime);
  }
}

void flush_all_send_buffers(std::vector<SendBuffer>& send_buffers, int comm_size, int my_rank, lci::device_t device,
                            const RedistributorOptions& options, RedistributorRuntime* runtime) {
  for (int i = 0; i < comm_size; ++i) {
    const int index = (my_rank + i) % comm_size;
    if (!send_buffers[index].empty()) {
      flush_send_buffer(send_buffers, index, my_rank, device, options, runtime);
    }
  }
}

} // namespace

void initialize_redistributor(RedistributorRuntime* runtime, const RedistributorOptions& options) {
  active_runtime = runtime;
  active_runtime_uses_upacket = options.use_upacket;
  runtime->send_counter = lci::alloc_counter();
  runtime->handler = lci::alloc_handler_x(am_handler).zero_copy_am(options.use_upacket)();
  runtime->rcomp = lci::register_rcomp(runtime->handler);
}

void finalize_redistributor(RedistributorRuntime* runtime) {
  if (runtime->rcomp != 0) {
    lci::deregister_rcomp(runtime->rcomp);
    runtime->rcomp = 0;
  }
  if (runtime->handler != nullptr) {
    lci::free_comp(&runtime->handler);
  }
  if (runtime->send_counter != nullptr) {
    lci::free_comp(&runtime->send_counter);
  }
  if (runtime->fallback_buffer != nullptr) {
    std::free(runtime->fallback_buffer);
    runtime->fallback_buffer = nullptr;
    runtime->fallback_buffer_bytes = 0;
  }
  active_runtime = nullptr;
  active_runtime_uses_upacket = true;
}

void allocate_fallback_buffers(RedistributorRuntime* runtime, int comm_size, int max_threads,
                               const RedistributorOptions& options) {
  if (options.use_upacket || runtime->fallback_buffer != nullptr) {
    return;
  }

  runtime->fallback_buffer_bytes = static_cast<size_t>(options.message_batch_size) * sizeof(KeyValue) *
                                   static_cast<size_t>(comm_size) * static_cast<size_t>(max_threads);
  runtime->fallback_buffer = std::malloc(runtime->fallback_buffer_bytes);
}

void reset_redistributor_iteration(RedistributorRuntime* runtime, std::atomic<KeyCount>* frequency_histogram) {
  runtime->frequency_histogram = frequency_histogram;
  runtime->received_count.store(0, std::memory_order_relaxed);
  lci::counter_set(runtime->send_counter, 0);
}

void redistribute_keys(const KeyValue* keys, KeyCount local_key_count, const int* bucket_to_rank, int bucket_shift,
                       KeyCount expected_recv_count, int comm_size, int my_rank,
                       const std::vector<lci::device_t>& devices, const RedistributorOptions& options,
                       RedistributorRuntime* runtime) {
  #pragma omp parallel
  {
    const int thread_id = omp_get_thread_num();
    std::vector<SendBuffer> send_buffers(comm_size);
    lci::device_t device = devices[thread_id % devices.size()];

    #pragma omp for nowait
    for (KeyCount i = 0; i < local_key_count; i++) {
      const int dest_rank = bucket_to_rank[keys[i] >> bucket_shift];
      send_key_to_processor(keys[i], dest_rank, send_buffers, comm_size, my_rank, device, options, runtime);
    }

    flush_all_send_buffers(send_buffers, comm_size, my_rank, device, options, runtime);

#ifdef A2A_TL_TIMERS
    TL_STEP_START(A2A_PROGRESS_WAIT);
#endif
    while (runtime->received_count.load(std::memory_order_relaxed) < static_cast<size_t>(expected_recv_count)) {
      lci::progress_x().device(device)();
    }
#ifdef A2A_TL_TIMERS
    TL_STEP_STOP(A2A_PROGRESS_WAIT);
    a2atl::publish_thread_stats(thread_id);
#endif
  }
}

} // namespace is_lci
