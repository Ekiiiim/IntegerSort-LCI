#pragma once

#include <lci.hpp>

#include <cstddef>
#include <vector>

namespace is_lci {

// Owns LCI initialization, device allocation, and the low-level collectives
// used by the benchmark pipeline.
class LciRuntime {
public:
  LciRuntime();
  ~LciRuntime();

  LciRuntime(const LciRuntime&) = delete;
  LciRuntime& operator=(const LciRuntime&) = delete;

  int rank() const;
  int rank_count() const;
  int max_threads() const;
  int threads_per_device() const;

  const std::vector<lci::device_t>& devices() const;
  const lci::device_t& control_device() const;

  void barrier() const;
  void broadcast_int(int* value, int root) const;
  void broadcast_bytes(void* data, size_t bytes, int root) const;

  template <typename ReduceOp>
  void reduce(const void* sendbuf, void* recvbuf, size_t count, size_t element_size, ReduceOp op, int root) const {
    lci::reduce_x(sendbuf, recvbuf, count, element_size, op, root).device(control_device())();
  }

private:
  int read_threads_per_device() const;
  void allocate_devices();
  void free_devices();

  int rank_ = 0;
  int rank_count_ = 0;
  int max_threads_ = 1;
  int threads_per_device_ = 1;
  std::vector<lci::device_t> devices_;
};

} // namespace is_lci
