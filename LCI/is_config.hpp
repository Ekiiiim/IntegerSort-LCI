#pragma once

/******************/
/* default values */
/******************/
#ifndef CLASS
#define CLASS 'S'
#define NUM_PROCS 1
#endif
#define MIN_PROCS 1
#define ONE 1

/*************/
/*  CLASS S  */
/*************/
#if CLASS == 'S'
#define TOTAL_KEYS_LOG_2 16
#define MAX_KEY_LOG_2 11
#define NUM_BUCKETS_LOG_2 9
#endif

/*************/
/*  CLASS W  */
/*************/
#if CLASS == 'W'
#define TOTAL_KEYS_LOG_2 20
#define MAX_KEY_LOG_2 16
#define NUM_BUCKETS_LOG_2 10
#endif

/*************/
/*  CLASS A  */
/*************/
#if CLASS == 'A'
#define TOTAL_KEYS_LOG_2 23
#define MAX_KEY_LOG_2 19
#define NUM_BUCKETS_LOG_2 10
#endif

/*************/
/*  CLASS B  */
/*************/
#if CLASS == 'B'
#define TOTAL_KEYS_LOG_2 25
#define MAX_KEY_LOG_2 21
#define NUM_BUCKETS_LOG_2 10
#endif

/*************/
/*  CLASS C  */
/*************/
#if CLASS == 'C'
#define TOTAL_KEYS_LOG_2 27
#define MAX_KEY_LOG_2 23
#define NUM_BUCKETS_LOG_2 10
#endif

/*************/
/*  CLASS D  */
/*************/
#if CLASS == 'D'
#define TOTAL_KEYS_LOG_2 29
#define MAX_KEY_LOG_2 27
#define NUM_BUCKETS_LOG_2 10
#undef MIN_PROCS
#define MIN_PROCS 4
#endif

/*************/
/*  CLASS E  */
/*************/
#if CLASS == 'E'
#define TOTAL_KEYS_LOG_2 29
#define MAX_KEY_LOG_2 31
#define NUM_BUCKETS_LOG_2 10
#undef MIN_PROCS
#define MIN_PROCS 64
#undef ONE
#define ONE 1L
#endif

/*******************************************************************
 * Defining MIN_PROCS avoids integer overflow for large problem
 * sizes without using a larger integer type for TOTAL_KEYS.
 * The actual total keys = TOTAL_KEYS * MIN_PROCS.
 *******************************************************************/
#define TOTAL_KEYS (1 << TOTAL_KEYS_LOG_2)
#define MAX_KEY (ONE << MAX_KEY_LOG_2)
#define NUM_BUCKETS (1 << NUM_BUCKETS_LOG_2)

#if CLASS == 'S'
#define MAX_PROCS 128
#else
#define MAX_PROCS 4096
#endif

#define MAX_ITERATIONS 10
#define TEST_ARRAY_SIZE 5

namespace is_lci {

using Count = int;

#if CLASS == 'D' || CLASS == 'E'
using Rank = long;
#else
using Rank = int;
#endif

struct ProblemConfig {
  char benchmark_class;
  int total_keys_log2;
  int max_key_log2;
  int num_buckets_log2;
  Rank total_keys;
  Rank max_key;
  int num_buckets;
  int min_processes;
  int max_processes;
  int iterations;
};

// Paper Step 1 setup: expose the NAS problem configuration selected by CLASS.
inline ProblemConfig problem_config() {
  return ProblemConfig{
      CLASS,
      TOTAL_KEYS_LOG_2,
      MAX_KEY_LOG_2,
      NUM_BUCKETS_LOG_2,
      static_cast<Rank>(TOTAL_KEYS) * static_cast<Rank>(MIN_PROCS),
      static_cast<Rank>(MAX_KEY),
      NUM_BUCKETS,
      MIN_PROCS,
      MAX_PROCS,
      MAX_ITERATIONS,
  };
}

} // namespace is_lci
