/*************************************************************************
 * Based on NAS benchmark
 *
 * Source logic: NPB 3.4 MPI IS timer ids, timer.flag handling, total timer,
 * optional rank/communication/verification timers, and min/max/average timer
 * summary.
 *
 * LCI adaptations that keep the benchmark logic unchanged:
 * - stores the timer-enabled state inside this module instead of using the
 *   NAS global timeron variable and TIMER_START/TIMER_STOP macros;
 * - keeps the NAS c_timers backend and timer labels;
 * - uses LCI reductions instead of MPI_Reduce to compute the same timer
 *   summary values.
 *************************************************************************/

#include "nas/timers.hpp"

#include "c_timers.h"

#ifndef NO_MTIMERS
#include "communication/reductions.hpp"

#include <omp.h>

#include <cstdio>
#endif

namespace is_lci {

static int g_timer_enabled = 0;

#ifdef NO_MTIMERS

int check_timer_flag_from_file() {
  return 0;
}

void set_timer_enabled(int timeron) {
  (void)timeron;
  g_timer_enabled = 0;
}

bool timer_enabled() {
  return false;
}

void clear_timers() {}

void start_total_timer() {
  timer_clear(T_TOTAL);
  timer_start(T_TOTAL);
}

void stop_total_timer() {
  timer_stop(T_TOTAL);
}

double read_total_timer() {
  return timer_read(T_TOTAL);
}

double read_rank_timer() {
  return 0.0;
}

void timer_start_if_enabled(int timer_id) {
  (void)timer_id;
}

void timer_stop_if_enabled(int timer_id) {
  (void)timer_id;
}

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

int check_timer_flag_from_file() {
  return check_timer_flag();
}

void set_timer_enabled(int timeron) {
  g_timer_enabled = timeron;
}

bool timer_enabled() {
  return g_timer_enabled != 0;
}

void clear_timers() {
  #pragma omp parallel for schedule(static)
  for (int i = 1; i <= T_LAST; i++) {
    timer_clear(i);
  }
}

void start_total_timer() {
  timer_clear(T_TOTAL);
  timer_start(T_TOTAL);
}

void stop_total_timer() {
  timer_stop(T_TOTAL);
}

double read_total_timer() {
  return timer_read(T_TOTAL);
}

double read_rank_timer() {
  return timer_read(T_RANK);
}

void timer_start_if_enabled(int timer_id) {
  if (timer_enabled()) {
    timer_start(timer_id);
  }
}

void timer_stop_if_enabled(int timer_id) {
  if (timer_enabled()) {
    timer_stop(timer_id);
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
