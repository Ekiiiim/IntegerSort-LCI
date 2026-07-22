#pragma once

#include "integer_sort/types.hpp"

namespace is_lci {

// NAS IS input-generation constants and run-size rules from the MPI baseline.
constexpr double NAS_RANDOM_SEED = 314159265.00;
constexpr double NAS_RANDOM_MULTIPLIER = 1220703125.00;

bool is_process_count_in_range(int process_count);
int greatest_power_of_two_at_most(int process_count);
bool should_abort_for_non_power_of_two_process_count(const char* npb_nprocs_strict_env);

KeyCount compute_local_key_count(int comm_size);
KeyCount compute_work_buffer_size(int comm_size, KeyCount local_key_count);
long compute_total_random_numbers();

void apply_iteration_key_changes(KeyValue* keys, int iteration, int my_rank);

} // namespace is_lci
