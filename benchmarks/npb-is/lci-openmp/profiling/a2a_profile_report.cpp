#include "profiling/a2a_profile_report.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

namespace is_lci {
namespace {

struct ProfileOperation {
  const char* label;
  const lci_irregular::AmOperationProfile lci_irregular::AmWorkerProfile::* values;
};

constexpr std::array<ProfileOperation, 4> operations = {{
    {"flush_send", &lci_irregular::AmWorkerProfile::flush},
    {"progress_wait", &lci_irregular::AmWorkerProfile::progress},
    {"am_copy", &lci_irregular::AmWorkerProfile::remote_receive},
    {"self_copy", &lci_irregular::AmWorkerProfile::loopback_receive},
}};

void print_per_worker(const lci_irregular::AmExchangeProfile& profile, int iteration, int rank) {
  std::printf("A2A per-worker profile (iteration %d, rank %d):\n", iteration, rank);
  std::printf("  worker  operation         calls    bytes         time_ms\n");
  for (size_t worker = 0; worker < profile.workers.size(); ++worker) {
    for (const auto& operation : operations) {
      const auto& values = profile.workers[worker].*(operation.values);
      std::printf("  %6zu  %-15s %8llu  %12llu  %10.3f\n", worker, operation.label,
                  static_cast<unsigned long long>(values.calls), static_cast<unsigned long long>(values.payload_bytes),
                  static_cast<double>(values.elapsed_nanoseconds) / 1e6);
    }
  }
}

void print_minmax(const lci_irregular::AmExchangeProfile& profile, int iteration, int rank) {
  if (profile.workers.empty()) {
    return;
  }

  std::printf("\nA2A per-operation min/max across workers (iteration %d, rank %d):\n", iteration, rank);
  std::printf("  operation       field        min(worker)           max(worker)\n");
  for (const auto& operation : operations) {
    uint64_t min_calls = std::numeric_limits<uint64_t>::max();
    uint64_t max_calls = 0;
    uint64_t min_bytes = std::numeric_limits<uint64_t>::max();
    uint64_t max_bytes = 0;
    uint64_t min_nanoseconds = std::numeric_limits<uint64_t>::max();
    uint64_t max_nanoseconds = 0;
    size_t min_calls_worker = 0;
    size_t max_calls_worker = 0;
    size_t min_bytes_worker = 0;
    size_t max_bytes_worker = 0;
    size_t min_time_worker = 0;
    size_t max_time_worker = 0;

    for (size_t worker = 0; worker < profile.workers.size(); ++worker) {
      const auto& values = profile.workers[worker].*(operation.values);
      if (values.calls < min_calls) {
        min_calls = values.calls;
        min_calls_worker = worker;
      }
      if (values.calls > max_calls) {
        max_calls = values.calls;
        max_calls_worker = worker;
      }
      if (values.payload_bytes < min_bytes) {
        min_bytes = values.payload_bytes;
        min_bytes_worker = worker;
      }
      if (values.payload_bytes > max_bytes) {
        max_bytes = values.payload_bytes;
        max_bytes_worker = worker;
      }
      if (values.elapsed_nanoseconds < min_nanoseconds) {
        min_nanoseconds = values.elapsed_nanoseconds;
        min_time_worker = worker;
      }
      if (values.elapsed_nanoseconds > max_nanoseconds) {
        max_nanoseconds = values.elapsed_nanoseconds;
        max_time_worker = worker;
      }
    }

    std::printf("  %-15s %-10s %12llu (w=%zu)   %12llu (w=%zu)\n", operation.label, "calls",
                static_cast<unsigned long long>(min_calls), min_calls_worker,
                static_cast<unsigned long long>(max_calls), max_calls_worker);
    std::printf("  %-15s %-10s %12llu (w=%zu)   %12llu (w=%zu)\n", operation.label, "bytes",
                static_cast<unsigned long long>(min_bytes), min_bytes_worker,
                static_cast<unsigned long long>(max_bytes), max_bytes_worker);
    std::printf("  %-15s %-10s %12.3f (w=%zu)   %12.3f (w=%zu)\n", operation.label, "time_ms",
                static_cast<double>(min_nanoseconds) / 1e6, min_time_worker, static_cast<double>(max_nanoseconds) / 1e6,
                max_time_worker);
  }
  std::printf("\n");
}

} // namespace

void print_a2a_profile_report(const lci_irregular::AmExchangeProfile& profile, int iteration, int rank,
                              KeyCount local_key_count, double rank_time) {
  if (!profile.enabled) {
    return;
  }

  if (rank == 0) {
    print_per_worker(profile, iteration, rank);
    print_minmax(profile, iteration, rank);
  }

  // The legacy progress_wait label now represents generic user-visible
  // progress calls and wait iterations, excluding internal send retries.
  const double progress_seconds = static_cast<double>(profile.aggregate.progress.elapsed_nanoseconds) / 1e9;
  std::fprintf(stdout,
               "LB {\"iter\":%d,\"rank\":%d,\"total_local_keys\":%lld,"
               "\"cumulative_rcomp\":%.6f,\"a2a_progress_wait\":%.6f}\n",
               iteration, rank, static_cast<long long>(local_key_count), rank_time, progress_seconds);
  std::fflush(stdout);
}

} // namespace is_lci
