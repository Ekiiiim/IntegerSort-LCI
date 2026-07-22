#pragma once

#include <lci-irregular/detail/profile_types.hpp>

#include <lci.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lci_irregular {

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
template <typename Record, typename ReceiveBatch, typename IsDone> class AmExchange;

namespace detail {
class AmExchangeStateBase;
void am_handler(lci::status_t status) noexcept;
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
  // those workers are scheduled.
  template <typename Record, typename ReceiveBatch, typename IsDone>
  AmExchange<Record, ReceiveBatch, IsDone> am_exchange_start(ReceiveBatch receive_batch, IsDone is_done,
                                                             AmExchangeOptions options = {});

  // Blocking active-message exchange for fixed-size records. The runtime packs
  // records into aggregated AM payloads; callers provide routing, receive, and
  // completion logic.
  template <typename Record, typename RouteRecord, typename ReceiveBatch>
  AmExchangeProfile am_exchange_counted(const Record* records, size_t count, RouteRecord route_record,
                                        ReceiveBatch receive_batch, size_t expected_recv_count,
                                        AmExchangeOptions options = {});

  // General completion variant. is_done() must become true only after all
  // application-expected inbound records/messages have been processed.
  template <typename Record, typename RouteRecord, typename ReceiveBatch, typename IsDone>
  AmExchangeProfile am_exchange_until(const Record* records, size_t count, RouteRecord route_record,
                                      ReceiveBatch receive_batch, IsDone is_done, AmExchangeOptions options = {});

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
  lci::comp_t am_handler_ = nullptr;
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
