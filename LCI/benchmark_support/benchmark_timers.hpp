#pragma once

#include "nas/timer_rules.hpp"

#include <lci.hpp>

#include <vector>

namespace is_lci {

void timer_start_if_enabled(int timer_id, int timeron);
void timer_stop_if_enabled(int timer_id, int timeron);

int check_timer_flag_from_file();
void clear_timers();
void print_timer_summary(int comm_size, const std::vector<lci::device_t>& devices);

} // namespace is_lci
