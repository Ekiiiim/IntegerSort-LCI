#include "runtime/lci_runtime.hpp"

#include <omp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace is_lci {

LciRuntime::LciRuntime() {
  setvbuf(stderr, nullptr, _IONBF, 0);

  lci::g_runtime_init_x().alloc_default_device(false)();
  rank_ = lci::get_rank_me();
  world_size_ = lci::get_rank_n();
  max_threads_ = omp_get_max_threads();
  threads_per_device_ = read_threads_per_device();
  allocate_devices();
  barrier();
}

LciRuntime::~LciRuntime() {
  free_devices();
  lci::g_runtime_fina();
}

int LciRuntime::rank() const {
  return rank_;
}

int LciRuntime::world_size() const {
  return world_size_;
}

int LciRuntime::max_threads() const {
  return max_threads_;
}

int LciRuntime::threads_per_device() const {
  return threads_per_device_;
}

const std::vector<lci::device_t>& LciRuntime::devices() const {
  return devices_;
}

const lci::device_t& LciRuntime::control_device() const {
  return devices_[0];
}

void LciRuntime::barrier() const {
  lci::barrier_x().device(control_device())();
}

void LciRuntime::broadcast_int(int* value, int root) const {
  broadcast_bytes(value, sizeof(int), root);
}

void LciRuntime::broadcast_bytes(void* data, size_t bytes, int root) const {
  lci::broadcast_x(data, bytes, root).device(control_device())();
}

int LciRuntime::read_threads_per_device() const {
  const char* env_val = getenv("NUM_THREADS_PER_DEVICE");
  if (!env_val) {
    if (rank_ == 0) {
      fprintf(stderr, "[Warning] NUM_THREADS_PER_DEVICE not set, using 1\n");
    }
    return 1;
  }

  int threads_per_device = atoi(env_val);
  if (threads_per_device <= 0 || threads_per_device > max_threads_) {
    if (rank_ == 0) {
      fprintf(stderr, "[Warning] Invalid NUM_THREADS_PER_DEVICE value %d, using 1 instead\n", threads_per_device);
    }
    return 1;
  }
  return threads_per_device;
}

void LciRuntime::allocate_devices() {
  int num_devices = max_threads_ / threads_per_device_;

  size_t npackets = lci::get_default_packet_pool().get_attr_npackets();
  size_t max_nrecvs_per_device = std::min(npackets / 8 / num_devices, 4096UL);
  size_t max_nsends_per_device = std::min(npackets / 4 / world_size_ / num_devices, 64UL);
  max_nsends_per_device = std::max(max_nsends_per_device, 4UL);

  devices_.reserve(num_devices);
  for (int i = 0; i < num_devices; ++i) {
    devices_.push_back(
        lci::alloc_device_x().net_max_sends(max_nsends_per_device).net_max_recvs(max_nrecvs_per_device)());
  }
}

void LciRuntime::free_devices() {
  for (auto& device : devices_) {
    lci::free_device(&device);
  }
  devices_.clear();
}

} // namespace is_lci
