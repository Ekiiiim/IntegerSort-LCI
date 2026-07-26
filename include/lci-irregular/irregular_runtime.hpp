#pragma once

#include <lci-irregular/detail/profile_types.hpp>

#include <lci.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lci_irregular {

// A callback-scoped typed view over an active-message payload. The underlying
// packet contains byte storage, so records are loaded by value instead of being
// exposed through a Record pointer.
template <typename Record> class RecordBatchView {
  static_assert(std::is_trivially_copyable<Record>::value, "Record must be trivially copyable");
  static_assert(std::is_trivially_default_constructible<Record>::value,
                "Record must be trivially default constructible under C++17");
  static_assert(std::is_trivially_move_constructible<Record>::value,
                "Record must be trivially move constructible under C++17");

public:
  RecordBatchView(const void* bytes, size_t count) noexcept
      : bytes_(static_cast<const unsigned char*>(bytes)), count_(count) {}

  size_t size() const noexcept {
    return count_;
  }

  bool empty() const noexcept {
    return count_ == 0;
  }

  // Requires index < size().
  Record operator[](size_t index) const noexcept {
    Record record{};
    std::memcpy(&record, bytes_ + index * sizeof(Record), sizeof(Record));
    return record;
  }

private:
  const unsigned char* bytes_;
  size_t count_;
};

// Runtime-level LCI resource tuning. Applications decide how workers share
// devices; this library does not create or query application threads.
struct IrregularRuntimeOptions {
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

class IrregularRuntime;

namespace detail {
class AmExchangeStateBase;
class AmRuntimeStateBase;
void dispatch_am_message(lci::status_t status) noexcept;
const std::vector<lci::device_t>& devices(const IrregularRuntime& runtime);
const lci::device_t& control_device(const IrregularRuntime& runtime);
lci::rcomp_t remote_completion(const IrregularRuntime& runtime);
uint32_t register_exchange(IrregularRuntime& runtime, AmExchangeStateBase* state);
void deregister_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
AmExchangeStateBase* find_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
void submit_profile(IrregularRuntime& runtime, AmProfile profile);
} // namespace detail

class IrregularRuntime {
public:
  explicit IrregularRuntime(IrregularRuntimeOptions options = {});
  ~IrregularRuntime();

  IrregularRuntime(const IrregularRuntime&) = delete;
  IrregularRuntime& operator=(const IrregularRuntime&) = delete;

  int rank() const;
  int rank_count() const;
  int device_count() const;
  bool profiling_enabled() const noexcept;
  size_t profiling_worker_count() const noexcept;
  void write_profiles();

  void barrier() const;
  void broadcast_int(int* value, int root) const;
  void broadcast_bytes(void* data, size_t bytes, int root) const;
  void progress() const;
  void progress(size_t worker_index) const;

  template <typename ReduceOp>
  void reduce(const void* sendbuf, void* recvbuf, size_t count, size_t element_size, ReduceOp op, int root) const {
    lci::reduce_x(sendbuf, recvbuf, count, element_size, op, root).device(control_device())();
  }

  // Register the receive logic for one typed active-message phase. A runtime
  // supports one active phase at a time; call quiet() and clear_am_handler()
  // before replacing a phase that has posted sends.
  template <typename Record, typename AmHandler> void set_am_handler(AmHandler am_handler, AmOptions options = {});
  void clear_am_handler();

  // Send one typed record through runtime-owned per-worker aggregation buffers.
  // Concurrent workers must use distinct stable worker indices.
  template <typename Record> void post_am(size_t worker_index, int dest_rank, const Record& record);
  void flush_remaining_buffers(size_t worker_index);
  void quiet(size_t worker_index);

  size_t received_record_count() const noexcept;
  bool local_sends_drained() const noexcept;
  AmProfile am_profile() const;

private:
  friend const std::vector<lci::device_t>& detail::devices(const IrregularRuntime& runtime);
  friend const lci::device_t& detail::control_device(const IrregularRuntime& runtime);
  friend lci::rcomp_t detail::remote_completion(const IrregularRuntime& runtime);
  friend uint32_t detail::register_exchange(IrregularRuntime& runtime, detail::AmExchangeStateBase* state);
  friend void detail::deregister_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
  friend detail::AmExchangeStateBase* detail::find_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
  friend void detail::submit_profile(IrregularRuntime& runtime, AmProfile profile);

  const lci::device_t& control_device() const;
  void allocate_devices(const IrregularRuntimeOptions& options);
  void free_devices();

  uint32_t register_exchange(detail::AmExchangeStateBase* state);
  void deregister_exchange(uint32_t exchange_id);
  detail::AmExchangeStateBase* find_exchange(uint32_t exchange_id);
  detail::AmRuntimeStateBase& active_am_state();
  const detail::AmRuntimeStateBase& active_am_state() const;
  void validate_worker_index(size_t worker_index) const;

  int rank_ = 0;
  int rank_count_ = 0;
  std::vector<lci::device_t> devices_;
  lci::comp_t am_dispatch_handler_ = nullptr;
  lci::rcomp_t am_rcomp_ = 0;
  uint32_t next_exchange_id_ = 1;
  std::mutex exchange_mutex_;
  std::unordered_map<uint32_t, detail::AmExchangeStateBase*> exchanges_;
  std::unique_ptr<detail::AmRuntimeStateBase> active_am_state_;
  ProfilingOptions profiling_options_;
  std::mutex profile_mutex_;
  std::mutex profile_write_mutex_;
  std::vector<AmProfile> pending_profiles_;
  bool profile_output_started_ = false;
};

namespace detail {
IrregularRuntime& active_runtime();
} // namespace detail

} // namespace lci_irregular

#include <lci-irregular/detail/am_exchange.hpp>
