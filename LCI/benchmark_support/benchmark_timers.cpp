#include "benchmark_support/benchmark_timers.hpp"

#include "communication/reductions.hpp"
#include "c_timers.h"

#include <omp.h>

#include <cstdio>
#include <cstring>

namespace is_lci {

void timer_start_if_enabled(int timer_id, int timeron) {
#ifdef NO_MTIMERS
  (void)timer_id;
  (void)timeron;
#else
  if (timeron) {
    timer_start(timer_id);
  }
#endif
}

void timer_stop_if_enabled(int timer_id, int timeron) {
#ifdef NO_MTIMERS
  (void)timer_id;
  (void)timeron;
#else
  if (timeron) {
    timer_stop(timer_id);
  }
#endif
}

int check_timer_flag_from_file() {
  return check_timer_flag();
}

void clear_timers() {
#ifndef NO_MTIMERS
#pragma omp parallel for schedule(static)
  for (int i = 1; i <= T_LAST; i++) {
    timer_clear(i);
  }
#endif
}

void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices) {
  double t1[T_LAST + 1], tmin[T_LAST + 1], tsum[T_LAST + 1], tmax[T_LAST + 1];
  char t_recs[T_LAST + 1][9];

#pragma omp parallel for schedule(static)
  for (int i = 0; i <= T_LAST; i++) {
    t1[i] = timer_read(i);
  }

  lci::reduce_x(t1, tmin, T_LAST + 1, sizeof(double), min_op, 0).device(devices[0])();
  lci::reduce_x(t1, tsum, T_LAST + 1, sizeof(double), sum_op_double, 0).device(devices[0])();
  lci::reduce_x(t1, tmax, T_LAST + 1, sizeof(double), max_op, 0).device(devices[0])();

  if (lci::get_rank_me() == 0) {
    strcpy(t_recs[T_TOTAL], "total");
    strcpy(t_recs[T_RANK], "rcomp");
    strcpy(t_recs[T_RCOMM], "rcomm");
    strcpy(t_recs[T_VERIFY], "verify");
    strcpy(t_recs[T_ALLTOALL], "atallv");
    strcpy(t_recs[T_RANK_1], "rcomp1");
    strcpy(t_recs[T_RANK_2], "rcomp2");
    strcpy(t_recs[T_RANK_3], "rcomp3");
    strcpy(t_recs[T_RANK_1_1], "rcomp1.1");
    printf(" nprocs = %6d          minimum     maximum     average\n", comm_size);
    for (int i = 0; i <= T_LAST; i++) {
      printf(" timer %2d (%-8s):  %10.4f  %10.4f  %10.4f\n", i + 1, t_recs[i], tmin[i], tmax[i],
             tsum[i] / static_cast<double>(comm_size));
    }
    printf("\n");
  }
}

} // namespace is_lci
