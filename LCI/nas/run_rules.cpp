/*************************************************************************
 * Based on NAS benchmark
 *
 * Source logic: NPB 3.4 MPI IS process-count rules, power-of-two active
 * communicator selection, work-buffer sizing, random-stream length, and
 * per-iteration key changes.
 *
 * LCI adaptations that keep the benchmark logic unchanged:
 * - splits the main-program checks and formulas into small named helpers;
 * - lets the LCI driver perform broadcast/exit behavior instead of calling
 *   MPI_Bcast, MPI_Abort, or MPI_Comm_split here;
 * - uses the project KeyValue/KeyCount aliases for the NAS integer types.
 *************************************************************************/

#include "nas/run_rules.hpp"

#include <cstring>

namespace is_lci {

bool is_process_count_in_range(int process_count) {
  return process_count >= MIN_PROCS && process_count <= MAX_PROCS;
}

int greatest_power_of_two_at_most(int process_count) {
  int active_count = 1;
  for (; active_count < process_count; active_count *= 2) {
  }
  if (active_count > process_count) {
    active_count /= 2;
  }
  return active_count;
}

bool should_abort_for_non_power_of_two_process_count(const char* npb_nprocs_strict_env) {
  if (npb_nprocs_strict_env && *npb_nprocs_strict_env) {
    if (strchr("nNfF-", *npb_nprocs_strict_env) || strcmp(npb_nprocs_strict_env, "0") == 0) {
      return false;
    }
    if (strcmp(npb_nprocs_strict_env, "off") == 0 || strcmp(npb_nprocs_strict_env, "OFF") == 0) {
      return false;
    }
  }
  return true;
}

KeyCount compute_local_key_count(int comm_size) {
  return (TOTAL_KEYS / comm_size) * MIN_PROCS;
}

KeyCount compute_work_buffer_size(int comm_size, KeyCount local_key_count) {
  if (comm_size < 256) {
    return 3 * local_key_count / 2;
  }
  if (comm_size < 512) {
    return 5 * local_key_count / 2;
  }
  if (comm_size < 1024) {
    return 4 * local_key_count;
  }
  return 13 * local_key_count / 2;
}

long compute_total_random_numbers() {
  return 4 * static_cast<long>(TOTAL_KEYS) * MIN_PROCS;
}

void apply_iteration_key_changes(KeyValue* keys, int iteration, int my_rank) {
  if (my_rank == 0) {
    keys[iteration] = iteration;
    keys[iteration + MAX_ITERATIONS] = MAX_KEY - iteration;
  }
}

} // namespace is_lci
