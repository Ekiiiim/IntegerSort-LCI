#include "benchmark_support/results.hpp"

#include "nas/problem_config.hpp"
#include "nas/reporting_rules.hpp"

#include <cstdio>

extern "C" {
void c_print_results(const char* name, char _class, int n1, int n2, int n3, int niter, int nprocs_active,
                     int nprocs_total, double t, double mops, const char* optype, int passed_verification,
                     const char* npbversion, const char* compiletime, const char* mpicc, const char* clink,
                     const char* cmpi_lib, const char* cmpi_inc, const char* cflags, const char* clinkflags);
}

namespace is_lci {

void print_initial_status(int my_rank, int np_total, int comm_size, int use_upacket, int use_loopback) {
  if (my_rank != 0) {
    return;
  }

  printf("\n\n NAS Parallel Benchmarks 3.4 -- IS Benchmark\n\n");
  printf(" Size:  %ld  (class %c)\n", static_cast<long>(TOTAL_KEYS) * MIN_PROCS, CLASS);
  printf(" Iterations:   %d\n", MAX_ITERATIONS);
  printf(" Total number of processes:  %d\n", np_total);
  if (comm_size != np_total) {
    printf(" WARNING: Number of processes is not a power of two (%d active)\n", comm_size);
  }
  printf(use_upacket ? " Using upacket for buffer management\n" : " Using malloc/free for buffer management\n");
  printf(use_loopback ? " Loopback optimization: ENABLED\n" : " Loopback optimization: DISABLED\n");
}

void print_final_results(int comm_size, int np_total, double maxtime, int passed_verification) {
  int final_verification = final_verification_status(passed_verification, comm_size);
  c_print_results("IS", CLASS, static_cast<int>(TOTAL_KEYS), MIN_PROCS, 0, MAX_ITERATIONS, comm_size, np_total, maxtime,
                  compute_keys_ranked_mops(maxtime), "keys ranked", final_verification, NPBVERSION, COMPILETIME, MPICC,
                  CLINK, CMPI_LIB, CMPI_INC, CFLAGS, CLINKFLAGS);
}

} // namespace is_lci
