#include "benchmark_support/results.hpp"

#include "nas/reporting.hpp"

#include <cstdio>

namespace is_lci {

void print_initial_status(int my_rank, int total_rank_count, int active_rank_count, int use_upacket, int use_loopback) {
  print_nas_initial_status(my_rank, total_rank_count, active_rank_count);

  if (my_rank != 0) {
    return;
  }

  printf(use_upacket ? " Using upacket for buffer management\n" : " Using malloc/free for buffer management\n");
  printf(use_loopback ? " Loopback optimization: ENABLED\n" : " Loopback optimization: DISABLED\n");
}

void print_final_results(int active_rank_count, int total_rank_count, double maxtime, int passed_verification) {
  print_nas_final_results(active_rank_count, total_rank_count, maxtime, passed_verification);
}

} // namespace is_lci
