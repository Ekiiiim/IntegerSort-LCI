#pragma once

#include <lci-irregular/am_profile.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lci_irregular::detail {

enum class ProfileOperation : size_t { flush = 0, progress, remote_receive, loopback_receive, count };

struct AtomicOperationProfile {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> records{0};
  std::atomic<uint64_t> payload_bytes{0};
  std::atomic<uint64_t> elapsed_nanoseconds{0};
};

struct AtomicWorkerProfile {
  std::array<AtomicOperationProfile, static_cast<size_t>(ProfileOperation::count)> operations;
};

inline AmOperationProfile& operation_profile(AmWorkerProfile& profile, ProfileOperation operation) {
  switch (operation) {
  case ProfileOperation::flush:
    return profile.flush;
  case ProfileOperation::progress:
    return profile.progress;
  case ProfileOperation::remote_receive:
    return profile.remote_receive;
  case ProfileOperation::loopback_receive:
    return profile.loopback_receive;
  case ProfileOperation::count:
    break;
  }
  std::terminate();
}

inline void snapshot_operation(const AtomicOperationProfile& source, AmOperationProfile& destination) {
  destination.calls = source.calls.load(std::memory_order_relaxed);
  destination.records = source.records.load(std::memory_order_relaxed);
  destination.payload_bytes = source.payload_bytes.load(std::memory_order_relaxed);
  destination.elapsed_nanoseconds = source.elapsed_nanoseconds.load(std::memory_order_relaxed);
}

inline uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point start) noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

class AmProfileRecorder {
public:
  AmProfileRecorder(bool enabled, size_t worker_count, uint64_t phase_sequence, std::string name)
      : enabled_(enabled), phase_sequence_(phase_sequence), name_(std::move(name)),
        workers_(enabled ? worker_count : 0) {}

  bool enabled() const noexcept {
    return enabled_;
  }

  void set_phase_sequence(uint64_t phase_sequence) noexcept {
    phase_sequence_ = phase_sequence;
  }

  void record(ProfileOperation operation, std::optional<size_t> worker_index, size_t records, size_t payload_bytes,
              uint64_t elapsed_nanoseconds) noexcept {
    if (!enabled_) {
      return;
    }

    record_operation(aggregate_.operations[static_cast<size_t>(operation)], records, payload_bytes,
                     elapsed_nanoseconds);
    if (worker_index && *worker_index < workers_.size()) {
      record_operation(workers_[*worker_index].operations[static_cast<size_t>(operation)], records, payload_bytes,
                       elapsed_nanoseconds);
    }
  }

  template <typename Function>
  void measure(ProfileOperation operation, std::optional<size_t> worker_index, size_t records, size_t payload_bytes,
               Function&& function) {
    if (!enabled_) {
      std::forward<Function>(function)();
      return;
    }

    const auto start = std::chrono::steady_clock::now();
    std::forward<Function>(function)();
    record(operation, worker_index, records, payload_bytes, elapsed_nanoseconds(start));
  }

  AmProfile snapshot() const {
    AmProfile result;
    result.enabled = enabled_;
    result.phase_sequence = phase_sequence_;
    result.name = name_;
    if (!enabled_) {
      return result;
    }

    snapshot_worker(aggregate_, result.aggregate);
    result.workers.resize(workers_.size());
    for (size_t i = 0; i < workers_.size(); ++i) {
      snapshot_worker(workers_[i], result.workers[i]);
    }
    return result;
  }

private:
  static void record_operation(AtomicOperationProfile& profile, size_t records, size_t payload_bytes,
                               uint64_t elapsed_nanoseconds) noexcept {
    profile.calls.fetch_add(1, std::memory_order_relaxed);
    profile.records.fetch_add(records, std::memory_order_relaxed);
    profile.payload_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
    profile.elapsed_nanoseconds.fetch_add(elapsed_nanoseconds, std::memory_order_relaxed);
  }

  static void snapshot_worker(const AtomicWorkerProfile& source, AmWorkerProfile& destination) {
    for (size_t i = 0; i < static_cast<size_t>(ProfileOperation::count); ++i) {
      const auto operation = static_cast<ProfileOperation>(i);
      snapshot_operation(source.operations[i], operation_profile(destination, operation));
    }
  }

  bool enabled_;
  uint64_t phase_sequence_;
  std::string name_;
  AtomicWorkerProfile aggregate_;
  std::vector<AtomicWorkerProfile> workers_;
};

inline std::optional<size_t>& current_progress_worker() noexcept {
  static thread_local std::optional<size_t> worker_index;
  return worker_index;
}

inline void set_progress_worker(std::optional<size_t> worker_index) noexcept {
  current_progress_worker() = worker_index;
}

inline std::optional<size_t> progress_worker() noexcept {
  return current_progress_worker();
}

class ProgressWorkerScope {
public:
  ProgressWorkerScope(bool enabled, std::optional<size_t> worker_index) noexcept : enabled_(enabled) {
    if (enabled_) {
      previous_ = progress_worker();
      set_progress_worker(worker_index);
    }
  }

  ~ProgressWorkerScope() {
    if (enabled_) {
      set_progress_worker(previous_);
    }
  }

  ProgressWorkerScope(const ProgressWorkerScope&) = delete;
  ProgressWorkerScope& operator=(const ProgressWorkerScope&) = delete;

private:
  bool enabled_;
  std::optional<size_t> previous_;
};

} // namespace lci_irregular::detail
