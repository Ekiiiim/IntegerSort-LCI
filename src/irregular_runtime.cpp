#include <lci-irregular/irregular_runtime.hpp>

#include <lci-irregular/detail/profiling.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

namespace lci_irregular {
namespace {

IrregularRuntime* g_active_runtime = nullptr;

} // namespace

IrregularRuntime::IrregularRuntime(IrregularRuntimeOptions options) : profiling_options_(options.profiling) {
  if (options.device_count <= 0) {
    throw std::invalid_argument("LCI irregular device_count must be positive");
  }
  if (profiling_options_.enabled && profiling_options_.worker_count == 0) {
    throw std::invalid_argument("profiling worker_count must be positive");
  }
  if (profiling_options_.file_prefix.empty() || profiling_options_.file_prefix.find('/') != std::string::npos ||
      profiling_options_.file_prefix.find('\\') != std::string::npos) {
    throw std::invalid_argument("profiling file_prefix must be a file-name prefix");
  }
  if (g_active_runtime != nullptr) {
    throw std::logic_error("IrregularRuntime supports only one live instance per process");
  }

  bool lci_initialized = false;
  try {
    lci::g_runtime_init_x().alloc_default_device(false)();
    lci_initialized = true;
    rank_ = lci::get_rank_me();
    rank_count_ = lci::get_rank_n();
    allocate_devices(options);
    am_dispatch_handler_ = lci::alloc_handler_x(detail::dispatch_am_message).zero_copy_am(true)();
    am_rcomp_ = lci::register_rcomp(am_dispatch_handler_);
    g_active_runtime = this;
    barrier();
  } catch (...) {
    if (g_active_runtime == this) {
      g_active_runtime = nullptr;
    }
    if (am_rcomp_ != 0) {
      lci::deregister_rcomp(am_rcomp_);
      am_rcomp_ = 0;
    }
    if (!am_dispatch_handler_.is_empty()) {
      lci::free_comp(&am_dispatch_handler_);
    }
    free_devices();
    if (lci_initialized) {
      lci::g_runtime_fina();
    }
    throw;
  }
}

IrregularRuntime::~IrregularRuntime() {
  if (active_am_state_) {
    if (active_am_state_->has_buffered_records()) {
      std::fprintf(stderr, "LCI irregular runtime destroyed with unflushed AM records\n");
      std::terminate();
    }
    if (!active_am_state_->local_sends_drained()) {
      std::fprintf(stderr, "LCI irregular runtime destroyed before local AM sends drained\n");
      std::terminate();
    }
    active_am_state_->finalize_profile(*this);
    active_am_state_.reset();
  }
  if (profiling_options_.enabled && !profiling_options_.output_directory.empty()) {
    try {
      write_profiles();
    } catch (const std::exception& error) {
      std::fprintf(stderr, "[lci-irregular] unable to write profiles: %s\n", error.what());
    }
  }
  if (am_rcomp_ != 0) {
    lci::deregister_rcomp(am_rcomp_);
    am_rcomp_ = 0;
  }
  if (!am_dispatch_handler_.is_empty()) {
    lci::free_comp(&am_dispatch_handler_);
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
  detail::ProgressWorkerScope worker_scope(profiling_options_.enabled, std::nullopt);
  for (const auto& device : devices_) {
    lci::progress_x().device(device)();
  }
}

void IrregularRuntime::progress(size_t worker_index) const {
  validate_worker_index(worker_index);
  detail::ProgressWorkerScope worker_scope(profiling_options_.enabled, worker_index);
  lci::progress_x().device(devices_[worker_index % devices_.size()])();
}

void IrregularRuntime::clear_am_handler() {
  if (!active_am_state_) {
    return;
  }
  if (active_am_state_->has_buffered_records()) {
    throw std::logic_error("LCI irregular AM handler cleared with unflushed records");
  }
  if (!active_am_state_->local_sends_drained()) {
    throw std::logic_error("LCI irregular AM handler cleared before local sends drained");
  }
  active_am_state_->finalize_profile(*this);
  active_am_state_.reset();
}

void IrregularRuntime::flush_remaining_buffers(size_t worker_index) {
  validate_worker_index(worker_index);
  active_am_state().flush_worker(worker_index);
}

void IrregularRuntime::quiet(size_t worker_index) {
  validate_worker_index(worker_index);
  active_am_state().quiet_worker(worker_index);
}

size_t IrregularRuntime::received_record_count() const noexcept {
  if (!active_am_state_) {
    return 0;
  }
  return active_am_state_->received_record_count();
}

bool IrregularRuntime::local_sends_drained() const noexcept {
  if (!active_am_state_) {
    return true;
  }
  return active_am_state_->local_sends_drained();
}

AmProfile IrregularRuntime::am_profile() const {
  return active_am_state().profile();
}

void IrregularRuntime::allocate_devices(const IrregularRuntimeOptions& options) {
  int num_devices = options.device_count;
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
  if (next_exchange_id_ == 0) {
    throw std::overflow_error("LCI irregular AM exchange id space exhausted");
  }
  const uint32_t exchange_id = next_exchange_id_;
  const auto result = exchanges_.emplace(exchange_id, state);
  if (!result.second) {
    throw std::logic_error("LCI irregular AM exchange id is already active");
  }
  ++next_exchange_id_;
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

detail::AmRuntimeStateBase& IrregularRuntime::active_am_state() {
  if (!active_am_state_) {
    throw std::logic_error("LCI irregular AM handler has not been registered");
  }
  return *active_am_state_;
}

const detail::AmRuntimeStateBase& IrregularRuntime::active_am_state() const {
  if (!active_am_state_) {
    throw std::logic_error("LCI irregular AM handler has not been registered");
  }
  return *active_am_state_;
}

void IrregularRuntime::validate_worker_index(size_t worker_index) const {
  if (devices_.empty()) {
    throw std::logic_error("LCI irregular runtime has no devices");
  }
  if (profiling_options_.enabled && worker_index >= profiling_options_.worker_count) {
    throw std::invalid_argument("LCI irregular worker index exceeds profiling worker_count");
  }
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

void submit_profile(IrregularRuntime& runtime, AmProfile profile) {
  std::lock_guard<std::mutex> lock(runtime.profile_mutex_);
  runtime.pending_profiles_.push_back(std::move(profile));
}

} // namespace detail

} // namespace lci_irregular
