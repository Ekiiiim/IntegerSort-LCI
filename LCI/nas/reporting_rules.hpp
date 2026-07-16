#pragma once

namespace is_lci {

// NAS IS result-reporting formulas from the MPI baseline.
int expected_verification_count(int comm_size);
int final_verification_status(int passed_verification, int comm_size);
double compute_keys_ranked_mops(double elapsed_seconds);

} // namespace is_lci
