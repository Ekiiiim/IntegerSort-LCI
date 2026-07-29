#include <lci-irregular/irregular_runtime.hpp>

#include <lci-irregular/detail/profiling.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>

namespace lci_irregular {
namespace {

Runtime* g_runtime_for_am = nullptr;
std::atomic<uint64_t> g_next_am_cache_generation{1};

} // namespace

Runtime::Runtime(RuntimeOptions options) : profiling_options_(options.profiling) {
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
  if (g_runtime_for_am != nullptr) {
    throw std::logic_error("Runtime supports only one live instance per process");
  }

  bool lci_initialized = false;
  try {
    lci::g_runtime_init_x().alloc_default_device(false)();
    lci_initialized = true;
    rank_ = lci::get_rank_me();
    rank_count_ = lci::get_rank_n();
    allocate_devices(options);
    am_send_completion_ = lci::alloc_counter();
    g_runtime_for_am = this;
    barrier();
  } catch (...) {
    if (g_runtime_for_am == this) {
      g_runtime_for_am = nullptr;
    }
    release_am_transport();
    free_devices();
    if (!am_send_completion_.is_empty()) {
      lci::free_comp(&am_send_completion_);
    }
    if (lci_initialized) {
      lci::g_runtime_fina();
    }
    throw;
  }
}

Runtime::~Runtime() {
  if (am_state_) {
    if (am_state_->has_buffered_records()) {
      std::fprintf(stderr, "LCI irregular runtime destroyed with unflushed AM records\n");
      std::terminate();
    }
    am_state_->finalize_profile(*this);
    release_am_transport();
    am_state_.reset();
    clear_am_dispatch();
  }
  if (profiling_options_.enabled && !profiling_options_.output_directory.empty()) {
    try {
      write_profiles();
    } catch (const std::exception& error) {
      std::fprintf(stderr, "[lci-irregular] unable to write profiles: %s\n", error.what());
    }
  }
  release_am_transport();
  free_devices();
  if (!am_send_completion_.is_empty()) {
    lci::free_comp(&am_send_completion_);
  }
  g_runtime_for_am = nullptr;
  lci::g_runtime_fina();
}

int Runtime::rank() const {
  return rank_;
}

int Runtime::rank_count() const {
  return rank_count_;
}

int Runtime::device_count() const {
  return static_cast<int>(devices_.size());
}

bool Runtime::profiling_enabled() const noexcept {
  return profiling_options_.enabled;
}

size_t Runtime::profiling_worker_count() const noexcept {
  return profiling_options_.worker_count;
}

const lci::device_t& Runtime::control_device() const {
  return devices_[0];
}

void Runtime::barrier() const {
  lci::barrier_x().device(control_device())();
}

void Runtime::broadcast(void* data, size_t bytes, int root) const {
  lci::broadcast_x(data, bytes, root).device(control_device())();
}

void Runtime::progress_worker(size_t worker_index) const {
  validate_worker_index(worker_index);
  detail::ProfileWorkerScope worker_scope(profiling_options_.enabled, worker_index);
  lci::progress_x().device(devices_[worker_index % devices_.size()])();
}

void Runtime::clear_am_handler() {
  if (!am_state_) {
    return;
  }
  if (am_state_->has_buffered_records()) {
    throw std::logic_error("LCI irregular AM handler cleared with unflushed records");
  }
  am_state_->finalize_profile(*this);
  release_am_transport();
  am_state_.reset();
  clear_am_dispatch();
}

WorkerHandler Runtime::get_worker_handler(size_t worker_index) {
  validate_worker_index(worker_index);
  return WorkerHandler(*this, worker_index);
}

void Runtime::flush_worker(size_t worker_index) {
  validate_worker_index(worker_index);
  require_am_state().flush_worker(worker_index);
}

size_t Runtime::recv_count() const noexcept {
  if (!am_state_) {
    return 0;
  }
  return am_state_->recv_count();
}

AmProfile Runtime::am_profile() const {
  return require_am_state().profile();
}

void Runtime::allocate_devices(const RuntimeOptions& options) {
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

void Runtime::free_devices() {
  for (auto& device : devices_) {
    lci::free_device(&device);
  }
  devices_.clear();
}

void Runtime::register_am_transport(bool use_upacket) {
  if (!lci_am_handler_.is_empty() || am_rcomp_ != 0) {
    throw std::logic_error("LCI irregular AM transport is already registered");
  }

  lci_am_handler_ = lci::alloc_handler_x(&Runtime::handle_incoming_am).zero_copy_am(use_upacket)();
  try {
    am_rcomp_ = lci::register_rcomp(lci_am_handler_);
  } catch (...) {
    lci::free_comp(&lci_am_handler_);
    throw;
  }
  am_receive_uses_upacket_ = use_upacket;
}

void Runtime::release_am_transport() noexcept {
  if (am_rcomp_ != 0) {
    lci::deregister_rcomp(am_rcomp_);
    am_rcomp_ = 0;
  }
  if (!lci_am_handler_.is_empty()) {
    lci::free_comp(&lci_am_handler_);
  }
  am_receive_uses_upacket_ = false;
}

void Runtime::clear_am_dispatch() noexcept {
  am_record_type_.reset();
  post_am_dispatch_ = nullptr;
}

uint64_t Runtime::allocate_am_cache_generation() {
  uint64_t next = g_next_am_cache_generation.load(std::memory_order_relaxed);
  while (true) {
    if (next == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("LCI irregular AM cache generation exhausted");
    }
    if (g_next_am_cache_generation.compare_exchange_weak(next, next + 1, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
      return next;
    }
  }
}

detail::AmRuntimeStateBase& Runtime::require_am_state() {
  if (!am_state_) {
    throw std::logic_error("LCI irregular AM handler has not been registered");
  }
  return *am_state_;
}

const detail::AmRuntimeStateBase& Runtime::require_am_state() const {
  if (!am_state_) {
    throw std::logic_error("LCI irregular AM handler has not been registered");
  }
  return *am_state_;
}

void Runtime::validate_worker_index(size_t worker_index) const {
  if (devices_.empty()) {
    throw std::logic_error("LCI irregular runtime has no devices");
  }
  if (worker_index == std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument("LCI irregular worker index is too large");
  }
  if (profiling_options_.enabled && worker_index >= profiling_options_.worker_count) {
    throw std::invalid_argument("LCI irregular worker index exceeds profiling worker_count");
  }
}

void Runtime::handle_incoming_am(lci::status_t status) noexcept {
  try {
    if (g_runtime_for_am == nullptr) {
      std::fprintf(stderr, "LCI irregular AM handler ran without an active runtime\n");
      std::abort();
    }
    Runtime& runtime = *g_runtime_for_am;

    runtime.require_am_state().receive_message(status.get_rank(), status.get_buffer(), status.get_size(), false);
    if (runtime.am_receive_uses_upacket_) {
      lci::put_upacket(status.get_buffer());
    } else {
      lci::get_allocator()->deallocate(status.get_buffer());
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LCI irregular AM handler terminated after exception: %s\n", error.what());
    std::terminate();
  } catch (...) {
    std::fprintf(stderr, "LCI irregular AM handler terminated after unknown exception\n");
    std::terminate();
  }
}

namespace detail {

const std::vector<lci::device_t>& devices(const Runtime& runtime) {
  return runtime.devices_;
}

const lci::device_t& control_device(const Runtime& runtime) {
  return runtime.control_device();
}

lci::comp_t send_completion(const Runtime& runtime) {
  return runtime.am_send_completion_;
}

lci::rcomp_t remote_completion(const Runtime& runtime) {
  return runtime.am_rcomp_;
}

void submit_profile(Runtime& runtime, AmProfile profile) {
  std::lock_guard<std::mutex> lock(runtime.profile_mutex_);
  runtime.pending_profiles_.push_back(std::move(profile));
}

} // namespace detail

void WorkerHandler::flush() {
  runtime_->flush_worker(worker_index_);
}

void WorkerHandler::progress() const {
  runtime_->progress_worker(worker_index_);
}

} // namespace lci_irregular
