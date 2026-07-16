#include "nas/reporting_rules.hpp"

#include "nas/problem_config.hpp"

namespace is_lci {

int expected_verification_count(int comm_size) {
  return 5 * MAX_ITERATIONS + comm_size;
}

int final_verification_status(int passed_verification, int comm_size) {
  return (passed_verification == expected_verification_count(comm_size)) ? passed_verification : 0;
}

double compute_keys_ranked_mops(double elapsed_seconds) {
  return (static_cast<double>(MAX_ITERATIONS) * TOTAL_KEYS * MIN_PROCS) / elapsed_seconds / 1000000.;
}

} // namespace is_lci
