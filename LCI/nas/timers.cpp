#include "nas/timers.hpp"

#ifndef NO_MTIMERS
#include "communication/reductions.hpp"
#include "c_timers.h"

#include <omp.h>

#include <cstdio>
#endif

namespace is_lci {

#ifdef NO_MTIMERS

void timer_start_if_enabled(int timer_id, int timeron) {
  (void)timer_id;
  (void)timeron;
}

void timer_stop_if_enabled(int timer_id, int timeron) {
  (void)timer_id;
  (void)timeron;
}

int check_timer_flag_from_file() {
  return 0;
}

void clear_timers() {}

void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices) {
  (void)comm_size;
  (void)devices;
}

#else

static const char* timer_label(int timer_id) {
  switch (timer_id) {
  case T_TOTAL:
    return "total";
  case T_RANK:
    return "rcomp";
  case T_RCOMM:
    return "rcomm";
  case T_VERIFY:
    return "verify";
  default:
    return "";
  }
}

void timer_start_if_enabled(int timer_id, int timeron) {
  if (timeron) {
    timer_start(timer_id);
  }
}

void timer_stop_if_enabled(int timer_id, int timeron) {
  if (timeron) {
    timer_stop(timer_id);
  }
}

int check_timer_flag_from_file() {
  return check_timer_flag();
}

void clear_timers() {
#pragma omp parallel for schedule(static)
  for (int i = 1; i <= T_LAST; i++) {
    timer_clear(i);
  }
}

void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices) {
  double t1[T_LAST + 1], tmin[T_LAST + 1], tsum[T_LAST + 1], tmax[T_LAST + 1];

#pragma omp parallel for schedule(static)
  for (int i = 0; i <= T_LAST; i++) {
    t1[i] = timer_read(i);
  }

  lci::reduce_x(t1, tmin, T_LAST + 1, sizeof(double), min_op, 0).device(devices[0])();
  lci::reduce_x(t1, tsum, T_LAST + 1, sizeof(double), sum_op_double, 0).device(devices[0])();
  lci::reduce_x(t1, tmax, T_LAST + 1, sizeof(double), max_op, 0).device(devices[0])();

  if (lci::get_rank_me() == 0) {
    printf(" nprocs = %6d          minimum     maximum     average\n", comm_size);
    for (int i = 0; i <= T_LAST; i++) {
      printf(" timer %2d (%-8s):  %10.4f  %10.4f  %10.4f\n", i + 1, timer_label(i), tmin[i], tmax[i],
             tsum[i] / static_cast<double>(comm_size));
    }
    printf("\n");
  }
}

#endif

} // namespace is_lci
