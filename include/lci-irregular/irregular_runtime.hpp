#pragma once

#include <lci-irregular/am_profile.hpp>

#include <lci.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace lci_irregular {

// Runtime-level LCI resource tuning. Applications decide how workers share
// devices; this library does not create or query application threads.
struct RuntimeOptions {
  int device_count = 1;
  size_t max_sends_per_device = 0;
  size_t max_recvs_per_device = 0;
  ProfilingOptions profiling;
};

// Active-message aggregation settings for the current runtime AM phase.
struct AmOptions {
  bool use_upacket = true;
  bool use_loopback = true;
  size_t batch_records = 0;
  std::string profile_name;
};

class Runtime;

// A lightweight proxy for one application-defined logical worker. Concurrent
// workers must use distinct stable indices.
class WorkerHandler {
public:
  // Append one record to this worker's destination-specific aggregation
  // buffer, posting the buffer when it becomes full.
  template <typename Record> void post_am(int dest_rank, const Record& record);

  // Post this worker's nonempty aggregation buffers without waiting for
  // network completion.
  void flush();

  // Advance the LCI device assigned to this worker.
  void progress() const;

private:
  friend class Runtime;

  WorkerHandler(Runtime& runtime, size_t worker_index) noexcept : runtime_(&runtime), worker_index_(worker_index) {}

  Runtime* runtime_;
  size_t worker_index_;
};

namespace detail {
class AmRuntimeStateBase;

class RecordTypeIdentity {
public:
  RecordTypeIdentity() = default;
  explicit RecordTypeIdentity(const std::type_info& type) noexcept : type_(&type) {}

  bool matches(const std::type_info& type) const noexcept {
    return type_ != nullptr && (type_ == &type || *type_ == type);
  }

  void reset() noexcept {
    type_ = nullptr;
  }

private:
  const std::type_info* type_ = nullptr;
};

template <typename Record> RecordTypeIdentity record_type_identity() noexcept {
  return RecordTypeIdentity(typeid(Record));
}

using ErasedAmPostFunction = void (*)(AmRuntimeStateBase&, size_t, int, const void*);
const std::vector<lci::device_t>& devices(const Runtime& runtime);
const lci::device_t& control_device(const Runtime& runtime);
lci::comp_t send_completion(const Runtime& runtime);
lci::rcomp_t remote_completion(const Runtime& runtime);
void submit_profile(Runtime& runtime, AmProfile profile);
} // namespace detail

class Runtime {
public:
  // Runtime construction and destruction are collective across participating
  // ranks. Only one Runtime may be live in a process.
  explicit Runtime(RuntimeOptions options = {});
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  int rank() const;
  int rank_count() const;
  int device_count() const;
  bool profiling_enabled() const noexcept;
  size_t profiling_worker_count() const noexcept;
  void write_profiles();

  void barrier() const;
  void broadcast(void* data, size_t bytes, int root) const;

  template <typename ReduceOp>
  void reduce(const void* sendbuf, void* recvbuf, size_t count, size_t element_size, ReduceOp op, int root) const {
    lci::reduce_x(sendbuf, recvbuf, count, element_size, op, root).device(control_device())();
  }

  // Collectively register one typed AM phase. The handler may additionally
  // accept an int source-rank argument and must support concurrent invocation.
  // Calling this while a phase is active is an error.
  template <typename Record, typename AmHandler> void set_am_handler(AmHandler am_handler, AmOptions options = {});

  // Locally end the active phase after every worker has flushed and the
  // application-defined receive-completion condition has been met.
  void clear_am_handler();

  // Worker handlers are phase-independent and may be reused across phases.
  WorkerHandler get_worker_handler(size_t worker_index);

  // Number of records whose receive handlers have completed in this phase.
  size_t recv_count() const noexcept;
  AmProfile am_profile() const;

private:
  friend class WorkerHandler;
  friend const std::vector<lci::device_t>& detail::devices(const Runtime& runtime);
  friend const lci::device_t& detail::control_device(const Runtime& runtime);
  friend lci::comp_t detail::send_completion(const Runtime& runtime);
  friend lci::rcomp_t detail::remote_completion(const Runtime& runtime);
  friend void detail::submit_profile(Runtime& runtime, AmProfile profile);

  static void handle_incoming_am(lci::status_t status) noexcept;
  const lci::device_t& control_device() const;
  void allocate_devices(const RuntimeOptions& options);
  void free_devices();
  void register_am_transport(bool use_upacket);
  void release_am_transport() noexcept;
  void clear_am_dispatch() noexcept;
  static uint64_t allocate_am_cache_generation();

  template <typename Record> void post_am_for_worker(size_t worker_index, int dest_rank, const Record& record);
  void flush_worker(size_t worker_index);
  void progress_worker(size_t worker_index) const;
  detail::AmRuntimeStateBase& require_am_state();
  const detail::AmRuntimeStateBase& require_am_state() const;
  void validate_worker_index(size_t worker_index) const;

  int rank_ = 0;
  int rank_count_ = 0;
  std::vector<lci::device_t> devices_;
  lci::comp_t am_send_completion_ = nullptr;
  lci::comp_t lci_am_handler_ = nullptr;
  lci::rcomp_t am_rcomp_ = 0;
  bool am_receive_uses_upacket_ = false;
  std::unique_ptr<detail::AmRuntimeStateBase> am_state_;
  detail::RecordTypeIdentity am_record_type_;
  detail::ErasedAmPostFunction post_am_dispatch_ = nullptr;
  ProfilingOptions profiling_options_;
  std::mutex profile_mutex_;
  std::mutex profile_write_mutex_;
  std::vector<AmProfile> pending_profiles_;
  bool profile_output_started_ = false;
};

} // namespace lci_irregular

#include <lci-irregular/detail/am_phase.hpp>
