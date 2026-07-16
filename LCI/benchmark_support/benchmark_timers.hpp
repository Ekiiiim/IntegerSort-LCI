#pragma once

#include <lci.hpp>

#include <vector>

namespace is_lci {

constexpr int T_TOTAL = 0;
constexpr int T_RANK = 1;
constexpr int T_RCOMM = 2;
constexpr int T_VERIFY = 3;
constexpr int T_ALLTOALL = 4;
constexpr int T_RANK_1 = 5;
constexpr int T_RANK_2 = 6;
constexpr int T_RANK_3 = 7;
constexpr int T_RANK_1_1 = 8;
constexpr int T_LAST = T_RANK_1_1;

void timer_start_if_enabled(int timer_id, int timeron);
void timer_stop_if_enabled(int timer_id, int timeron);

int check_timer_flag_from_file();
void clear_timers();
void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices);

} // namespace is_lci
