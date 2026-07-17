/*************************************************************************
 * NAS benchmark provenance
 *
 * Source logic: NPB 3.4 MPI IS initial benchmark printout, final
 * c_print_results call, verification-count rule, and MOPS formula.
 *
 * Adapted without changing the benchmark logic:
 * - exposes initial and final reporting as callable functions;
 * - receives active and total rank counts from the LCI driver, matching the
 *   NAS comm_size and np_total roles;
 * - keeps LCI-only option output outside this file in benchmark_support.
 *************************************************************************/

#include "nas/reporting.hpp"

#include "nas/problem_config.hpp"

#include <cstdio>

extern "C" {
void c_print_results(const char* name, char _class, int n1, int n2, int n3, int niter, int nprocs_active,
                     int nprocs_total, double t, double mops, const char* optype, int passed_verification,
                     const char* npbversion, const char* compiletime, const char* mpicc, const char* clink,
                     const char* cmpi_lib, const char* cmpi_inc, const char* cflags, const char* clinkflags);
}

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

void print_nas_initial_status(int my_rank, int total_rank_count, int active_rank_count) {
  if (my_rank != 0) {
    return;
  }

  printf("\n\n NAS Parallel Benchmarks 3.4 -- IS Benchmark\n\n");
  printf(" Size:  %ld  (class %c)\n", static_cast<long>(TOTAL_KEYS) * MIN_PROCS, CLASS);
  printf(" Iterations:   %d\n", MAX_ITERATIONS);
  printf(" Total number of processes:  %d\n", total_rank_count);
  if (active_rank_count != total_rank_count) {
    printf(" WARNING: Number of processes is not a power of two (%d active)\n", active_rank_count);
  }
}

void print_nas_final_results(int active_rank_count, int total_rank_count, double maxtime, int passed_verification) {
  int final_verification = final_verification_status(passed_verification, active_rank_count);
  c_print_results("IS", CLASS, static_cast<int>(TOTAL_KEYS), MIN_PROCS, 0, MAX_ITERATIONS, active_rank_count,
                  total_rank_count, maxtime, compute_keys_ranked_mops(maxtime), "keys ranked", final_verification,
                  NPBVERSION, COMPILETIME, MPICC, CLINK, CMPI_LIB, CMPI_INC, CFLAGS, CLINKFLAGS);
}

} // namespace is_lci
