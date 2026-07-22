#include <lci-irregular/irregular_runtime.hpp>

#include <lci-irregular/detail/profiling.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace lci_irregular {
namespace {

IrregularRuntime* g_active_runtime = nullptr;

} // namespace

IrregularRuntime::IrregularRuntime(IrregularRuntimeOptions options) : profiling_options_(options.profiling) {
  if (g_active_runtime != nullptr) {
    std::fprintf(stderr, "IrregularRuntime supports only one instance per process\n");
    std::abort();
  }

  lci::g_runtime_init_x().alloc_default_device(false)();
  rank_ = lci::get_rank_me();
  rank_count_ = lci::get_rank_n();
  if (options.device_count <= 0) {
    if (rank_ == 0) {
      std::fprintf(stderr, "[Warning] Invalid LCI irregular device_count value %d, using 1 instead\n",
                   options.device_count);
    }
    options.device_count = 1;
  }
  allocate_devices(options);
  am_handler_ = lci::alloc_handler_x(detail::am_handler).zero_copy_am(true)();
  am_rcomp_ = lci::register_rcomp(am_handler_);
  g_active_runtime = this;
  barrier();
}

IrregularRuntime::~IrregularRuntime() {
  if (am_rcomp_ != 0) {
    lci::deregister_rcomp(am_rcomp_);
    am_rcomp_ = 0;
  }
  if (!am_handler_.is_empty()) {
    lci::free_comp(&am_handler_);
  }
  free_devices();
  g_active_runtime = nullptr;
  lci::g_runtime_fina();
}

int IrregularRuntime::rank() const {
  return rank_;
}

int IrregularRuntime::rank_count() const {
  return rank_count_;
}

int IrregularRuntime::device_count() const {
  return static_cast<int>(devices_.size());
}

bool IrregularRuntime::profiling_enabled() const noexcept {
  return profiling_options_.enabled;
}

size_t IrregularRuntime::profiling_worker_count() const noexcept {
  return profiling_options_.worker_count;
}

const lci::device_t& IrregularRuntime::control_device() const {
  return devices_[0];
}

void IrregularRuntime::barrier() const {
  lci::barrier_x().device(control_device())();
}

void IrregularRuntime::broadcast_int(int* value, int root) const {
  broadcast_bytes(value, sizeof(int), root);
}

void IrregularRuntime::broadcast_bytes(void* data, size_t bytes, int root) const {
  lci::broadcast_x(data, bytes, root).device(control_device())();
}

void IrregularRuntime::progress() const {
  detail::ProgressWorkerScope worker_scope(std::nullopt);
  for (const auto& device : devices_) {
    lci::progress_x().device(device)();
  }
}

void IrregularRuntime::progress(size_t worker_index) const {
  detail::ProgressWorkerScope worker_scope(worker_index);
  lci::progress_x().device(devices_[worker_index % devices_.size()])();
}

void IrregularRuntime::allocate_devices(const IrregularRuntimeOptions& options) {
  int num_devices = std::max(1, options.device_count);
  size_t npackets = lci::get_default_packet_pool().get_attr_npackets();
  size_t max_recvs_per_device = options.max_recvs_per_device;
  size_t max_sends_per_device = options.max_sends_per_device;
  if (max_recvs_per_device == 0) {
    max_recvs_per_device = std::min(npackets / 8 / static_cast<size_t>(num_devices), 4096UL);
  }
  if (max_sends_per_device == 0) {
    max_sends_per_device =
        std::min(npackets / 4 / static_cast<size_t>(rank_count_) / static_cast<size_t>(num_devices), 64UL);
    max_sends_per_device = std::max(max_sends_per_device, 4UL);
  }

  devices_.reserve(num_devices);
  for (int i = 0; i < num_devices; ++i) {
    devices_.push_back(lci::alloc_device_x().net_max_sends(max_sends_per_device).net_max_recvs(max_recvs_per_device)());
  }
}

void IrregularRuntime::free_devices() {
  for (auto& device : devices_) {
    lci::free_device(&device);
  }
  devices_.clear();
}

uint32_t IrregularRuntime::register_exchange(detail::AmExchangeStateBase* state) {
  std::lock_guard<std::mutex> lock(exchange_mutex_);
  uint32_t exchange_id = next_exchange_id_++;
  exchanges_[exchange_id] = state;
  return exchange_id;
}

void IrregularRuntime::deregister_exchange(uint32_t exchange_id) {
  std::lock_guard<std::mutex> lock(exchange_mutex_);
  exchanges_.erase(exchange_id);
}

detail::AmExchangeStateBase* IrregularRuntime::find_exchange(uint32_t exchange_id) {
  std::lock_guard<std::mutex> lock(exchange_mutex_);
  auto iter = exchanges_.find(exchange_id);
  return iter == exchanges_.end() ? nullptr : iter->second;
}

namespace detail {

IrregularRuntime& active_runtime() {
  if (g_active_runtime == nullptr) {
    std::fprintf(stderr, "LCI irregular AM handler ran without an active runtime\n");
    std::abort();
  }
  return *g_active_runtime;
}

const std::vector<lci::device_t>& devices(const IrregularRuntime& runtime) {
  return runtime.devices_;
}

const lci::device_t& control_device(const IrregularRuntime& runtime) {
  return runtime.control_device();
}

lci::rcomp_t remote_completion(const IrregularRuntime& runtime) {
  return runtime.am_rcomp_;
}

uint32_t register_exchange(IrregularRuntime& runtime, AmExchangeStateBase* state) {
  return runtime.register_exchange(state);
}

void deregister_exchange(IrregularRuntime& runtime, uint32_t exchange_id) {
  runtime.deregister_exchange(exchange_id);
}

AmExchangeStateBase* find_exchange(IrregularRuntime& runtime, uint32_t exchange_id) {
  return runtime.find_exchange(exchange_id);
}

} // namespace detail

} // namespace lci_irregular
