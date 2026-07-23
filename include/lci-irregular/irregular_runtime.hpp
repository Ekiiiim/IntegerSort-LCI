#pragma once

#include <lci-irregular/detail/profile_types.hpp>

#include <lci.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
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

// Per-exchange active-message aggregation settings.
struct AmExchangeOptions {
  bool use_upacket = true;
  bool use_loopback = true;
  size_t batch_records = 0;
  std::string profile_name;
};

class IrregularRuntime;
template <typename Record, typename AmHandler, typename IsDone> class AmExchange;

namespace detail {
class AmExchangeStateBase;
void dispatch_am_message(lci::status_t status) noexcept;
const std::vector<lci::device_t>& devices(const IrregularRuntime& runtime);
const lci::device_t& control_device(const IrregularRuntime& runtime);
lci::rcomp_t remote_completion(const IrregularRuntime& runtime);
uint32_t register_exchange(IrregularRuntime& runtime, AmExchangeStateBase* state);
void deregister_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
AmExchangeStateBase* find_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
void submit_profile(IrregularRuntime& runtime, AmExchangeProfile profile);
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

  // Start a nonblocking active-message exchange. The returned handle owns the
  // exchange lifetime; applications create per-worker senders and decide how
  // those workers are scheduled. Participating ranks must create exchanges in
  // the same order and complete phase synchronization before the first send.
  // Every sender must be closed or destroyed before wait(), profile(), or the
  // exchange itself.
  template <typename Record, typename AmHandler, typename IsDone>
  AmExchange<Record, AmHandler, IsDone> am_exchange_start(AmHandler am_handler, IsDone is_done,
                                                          AmExchangeOptions options = {});

  // Run a blocking fork-join active-message exchange. The application launches
  // its workers in send_phase; each worker calls run_worker(worker_index,
  // produce), and produce calls am_send(destination_rank, record). The runtime
  // owns sender aggregation, flush, progress, completion, and profiling.
  // am_handler and is_done may run concurrently and must be thread-safe.
  // send_phase must not return until all run_worker calls have returned. Every
  // rank must invoke at least one run_worker, including ranks with no outgoing
  // records. produce and am_send must not escape their run_worker invocation.
  // All participating workers, including progress-only workers, must launch
  // concurrently. Concurrent workers require distinct indices that remain
  // stable for each invocation and map modulo device_count. With profiling
  // enabled, each index must be less than profiling.worker_count. A single
  // worker is valid only when it produces every outgoing record for its rank.
  // Dynamic tasks must use am_exchange_start().
  template <typename Record, typename AmHandler, typename IsDone, typename SendPhase>
  AmExchangeProfile am_exchange_until(AmHandler am_handler, IsDone is_done, SendPhase send_phase,
                                      AmExchangeOptions options = {});

private:
  friend const std::vector<lci::device_t>& detail::devices(const IrregularRuntime& runtime);
  friend const lci::device_t& detail::control_device(const IrregularRuntime& runtime);
  friend lci::rcomp_t detail::remote_completion(const IrregularRuntime& runtime);
  friend uint32_t detail::register_exchange(IrregularRuntime& runtime, detail::AmExchangeStateBase* state);
  friend void detail::deregister_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
  friend detail::AmExchangeStateBase* detail::find_exchange(IrregularRuntime& runtime, uint32_t exchange_id);
  friend void detail::submit_profile(IrregularRuntime& runtime, AmExchangeProfile profile);

  const lci::device_t& control_device() const;
  void allocate_devices(const IrregularRuntimeOptions& options);
  void free_devices();

  uint32_t register_exchange(detail::AmExchangeStateBase* state);
  void deregister_exchange(uint32_t exchange_id);
  detail::AmExchangeStateBase* find_exchange(uint32_t exchange_id);

  int rank_ = 0;
  int rank_count_ = 0;
  std::vector<lci::device_t> devices_;
  lci::comp_t am_dispatch_handler_ = nullptr;
  lci::rcomp_t am_rcomp_ = 0;
  uint32_t next_exchange_id_ = 1;
  std::mutex exchange_mutex_;
  std::unordered_map<uint32_t, detail::AmExchangeStateBase*> exchanges_;
  ProfilingOptions profiling_options_;
  std::mutex profile_mutex_;
  std::mutex profile_write_mutex_;
  std::vector<AmExchangeProfile> pending_profiles_;
  bool profile_output_started_ = false;
};

namespace detail {
IrregularRuntime& active_runtime();
} // namespace detail

} // namespace lci_irregular

#include <lci-irregular/detail/am_exchange.hpp>
