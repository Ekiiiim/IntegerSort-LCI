#pragma once

namespace is_lci {

// NAS IS result-reporting formulas and printout from the MPI baseline.
int expected_verification_count(int comm_size);
int final_verification_status(int passed_verification, int comm_size);
double compute_keys_ranked_mops(double elapsed_seconds);

void print_nas_initial_status(int my_rank, int total_rank_count, int active_rank_count);
void print_nas_final_results(int active_rank_count, int total_rank_count, double maxtime, int passed_verification);

} // namespace is_lci
