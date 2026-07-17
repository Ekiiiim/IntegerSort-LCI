#pragma once

#include <lci.hpp>

#include <vector>

namespace is_lci {

// NAS IS timer IDs and output labels from the NPB MPI implementation.
constexpr int T_TOTAL = 0;
constexpr int T_RANK = 1;
constexpr int T_RCOMM = 2;
constexpr int T_VERIFY = 3;
constexpr int T_LAST = T_VERIFY;

void timer_start_if_enabled(int timer_id, int timeron);
void timer_stop_if_enabled(int timer_id, int timeron);

int check_timer_flag_from_file();
void clear_timers();
void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices);

} // namespace is_lci
