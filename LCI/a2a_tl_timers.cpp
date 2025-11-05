// a2a_tl_timers.cpp
#include "a2a_tl_timers.hpp"

#ifdef A2A_TL_TIMERS

#include <chrono>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace a2atl {

using clock_steady = std::chrono::steady_clock;

// -------- Thread-local counters --------
thread_local uint64_t tl_calls[A2A_NUM_STEPS] = {0};
thread_local uint64_t tl_bytes[A2A_NUM_STEPS] = {0};
thread_local uint64_t tl_ns[A2A_NUM_STEPS]    = {0};
thread_local uint64_t tl_t0[A2A_NUM_STEPS]    = {0};

// -------- Global per-thread snapshot storage --------
static std::vector<ThreadStats> g_thread_stats;
std::vector<double> g_a2a_wait_per_thread;
double g_a2a_wait_total = 0.0;

void init(int num_threads) {
  g_thread_stats.assign(std::max(1, num_threads), {});
  g_a2a_wait_per_thread.assign(std::max(1, num_threads), 0.0);
}

void reset_thread_local(int num_threads) {
  std::memset(tl_calls, 0, sizeof(tl_calls));
  std::memset(tl_bytes, 0, sizeof(tl_bytes));
  std::memset(tl_ns,    0, sizeof(tl_ns));
  std::memset(tl_t0,    0, sizeof(tl_t0));
  g_a2a_wait_per_thread.assign(std::max(1, num_threads), 0.0);
}

static inline uint64_t now_ns() {
  return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
      clock_steady::now().time_since_epoch()).count();
}

void step_start(Step s) {
  tl_t0[s] = now_ns();
}

void step_stop(Step s) {
  const uint64_t t1 = now_ns();
  tl_ns[s] += (t1 - tl_t0[s]);
  tl_calls[s]++;
}

void add_bytes(Step s, uint64_t n) {
  tl_bytes[s] += n;
}

void publish_thread_stats(int thread_id) {
  g_a2a_wait_per_thread[thread_id] += (double)(tl_ns[A2A_PROGRESS_WAIT]) / 1e9;
  if (thread_id < 0) return;
  if ((size_t)thread_id >= g_thread_stats.size())
    g_thread_stats.resize((size_t)thread_id + 1);
  for (int s = 0; s < A2A_NUM_STEPS; ++s) {
    g_thread_stats[thread_id].calls[s] = tl_calls[s];
    g_thread_stats[thread_id].bytes[s] = tl_bytes[s];
    g_thread_stats[thread_id].ns[s]    = tl_ns[s];
  }
}

const std::vector<ThreadStats>& get_thread_stats() {
  return g_thread_stats;
}

const char* step_name(Step s) {
  static const char* names[A2A_NUM_STEPS] = {
    "flush_send", "progress_wait", "am_copy", "self_copy"
  };
  return (s >= 0 && s < A2A_NUM_STEPS) ? names[s] : "?";
}

// ------- Pretty print helpers (match your old prints) --------
void print_per_thread(int iteration, int rank) {
  const auto& stats = get_thread_stats();
  std::printf("A2A per-thread timers (iteration %d, rank %d):\n",
              iteration, rank);
  std::printf("  thread  step           calls    bytes         time_ms\n");
  for (int t = 0; t < (int)stats.size(); ++t) {
    for (int s = 0; s < A2A_NUM_STEPS; ++s) {
      const double ms = (double)stats[t].ns[s] / 1e6;
      std::printf("  %6d  %-12s %8llu  %12llu  %10.3f\n",
        t, step_name((Step)s),
        (unsigned long long)stats[t].calls[s],
        (unsigned long long)stats[t].bytes[s],
        ms);
    }
  }
}

void stamp_wait_total() {
  g_a2a_wait_total = 0.0;
  for (double t : g_a2a_wait_per_thread)
    g_a2a_wait_total += t;
}

void print_per_process(int iteration, int rank, long long total_local_keys, double cumulative_rcomp) {
  fprintf(stdout,
    "LB {\"iter\":%d,\"rank\":%d,\"total_local_keys\":%lld,"
    "\"cumulative_rcomp\":%.6f,\"a2a_progress_wait\":%.6f}\n",
    iteration, rank, total_local_keys, cumulative_rcomp, g_a2a_wait_total);
  fflush(stdout);
}

void print_minmax(int iteration, int rank) {
  const auto& stats = get_thread_stats();
  if (stats.empty()) return;

  struct AggU64 { uint64_t min_v, max_v; int tmin, tmax; };
  struct AggDbl { double   min_v, max_v; int tmin, tmax; };

  std::printf("\nA2A per-step min/max across threads (iteration %d, rank %d):\n",
              iteration, rank);
  std::printf("  step           field        min(thread)           max(thread)\n");

  for (int s = 0; s < A2A_NUM_STEPS; ++s) {
    AggU64 calls { stats[0].calls[s], stats[0].calls[s], 0, 0 };
    AggU64 bytes { stats[0].bytes[s], stats[0].bytes[s], 0, 0 };
    AggDbl time  { (double)stats[0].ns[s]/1e6, (double)stats[0].ns[s]/1e6, 0, 0 };

    for (int t = 1; t < (int)stats.size(); ++t) {
      const uint64_t c  = stats[t].calls[s];
      const uint64_t b  = stats[t].bytes[s];
      const double   ms = (double)stats[t].ns[s] / 1e6;

      if (c  < calls.min_v) { calls.min_v = c;  calls.tmin = t; }
      if (c  > calls.max_v) { calls.max_v = c;  calls.tmax = t; }
      if (b  < bytes.min_v) { bytes.min_v = b;  bytes.tmin = t; }
      if (b  > bytes.max_v) { bytes.max_v = b;  bytes.tmax = t; }
      if (ms < time.min_v)  { time.min_v  = ms; time.tmin = t; }
      if (ms > time.max_v)  { time.max_v  = ms; time.tmax = t; }
    }

    std::printf("  %-12s %-10s %12llu (t=%d)   %12llu (t=%d)\n",
      step_name((Step)s), "calls",
      (unsigned long long)calls.min_v, calls.tmin,
      (unsigned long long)calls.max_v, calls.tmax);

    std::printf("  %-12s %-10s %12llu (t=%d)   %12llu (t=%d)\n",
      step_name((Step)s), "bytes",
      (unsigned long long)bytes.min_v, bytes.tmin,
      (unsigned long long)bytes.max_v, bytes.tmax);

    std::printf("  %-12s %-10s %12.3f (t=%d)   %12.3f (t=%d)\n",
      step_name((Step)s), "time_ms",
      time.min_v, time.tmin,
      time.max_v, time.tmax);
  }
  std::printf("\n");
}

} // namespace a2atl

#endif // A2A_TL_TIMERS
