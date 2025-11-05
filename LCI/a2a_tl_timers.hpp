// a2a_tl_timers.hpp
#pragma once

// Turn the whole module into no-ops unless A2A_TL_TIMERS is defined
#ifdef A2A_TL_TIMERS

#include <cstdint>
#include <vector>

namespace a2atl {

// Steps you already track
enum Step : int {
  A2A_FLUSH_SEND = 0,
  A2A_PROGRESS_WAIT,
  A2A_AM_COPY,
  A2A_SELF_COPY,
  A2A_NUM_STEPS
};

struct ThreadStats {
  uint64_t calls[A2A_NUM_STEPS];
  uint64_t bytes[A2A_NUM_STEPS];
  uint64_t ns[A2A_NUM_STEPS];
};

// Must be called once before use (e.g., after you know the #threads)
void init(int num_threads);

// Optional: reset thread-local counters (rarely needed)
void reset_thread_local(int num_threads);

// Recorders used by macros below
void step_start(Step s);
void step_stop(Step s);
void add_bytes(Step s, uint64_t n);

// Publish the calling thread’s TL stats into the global vector slot
void publish_thread_stats(int thread_id);

// Accessors/printing helpers
const std::vector<ThreadStats>& get_thread_stats();
const char* step_name(Step s);

// Pretty print (what you do at rank 0 after a region)
void print_per_thread(int iteration, int rank);
void print_minmax(int iteration, int rank);

// process-level timer
void stamp_wait_total();
void print_per_process(int iteration, int rank, long long total_local_keys, double cumulative_rcomp);

} // namespace a2atl

enum {
  A2A_FLUSH_SEND  = ::a2atl::A2A_FLUSH_SEND,
  A2A_PROGRESS_WAIT = ::a2atl::A2A_PROGRESS_WAIT,
  A2A_AM_COPY     = ::a2atl::A2A_AM_COPY,
  A2A_SELF_COPY   = ::a2atl::A2A_SELF_COPY,
  A2A_NUM_STEPS   = ::a2atl::A2A_NUM_STEPS
};

// Convenience macros to mirror your current call sites
#define TL_STEP_START(step) ::a2atl::step_start(::a2atl::Step(step))
#define TL_STEP_STOP(step)  ::a2atl::step_stop(::a2atl::Step(step))
#define TL_ADD_BYTES(step, nbytes) ::a2atl::add_bytes(::a2atl::Step(step), (uint64_t)(nbytes))

#else  // -------- A2A_TL_TIMERS not defined: compile to no-ops ----------

namespace a2atl {
enum Step : int { A2A_NUM_STEPS = 0 };
inline void init(int) {}
inline void reset_thread_local(int) {}
inline void publish_thread_stats(int) {}
inline const char* step_name(int){ return ""; }
} // namespace a2atl

enum {
  A2A_FLUSH_SEND = 0,
  A2A_PROGRESS_WAIT = 0,
  A2A_AM_COPY = 0,
  A2A_SELF_COPY = 0,
  A2A_NUM_STEPS = 0
};

#define TL_STEP_START(step) do{}while(0)
#define TL_STEP_STOP(step)  do{}while(0)
#define TL_ADD_BYTES(step, nbytes) do{}while(0)

#endif // A2A_TL_TIMERS
