#pragma once

namespace lci_irregular {
class IrregularRuntime;
}

namespace is_lci {

// NAS IS timer IDs and output labels from the NPB MPI implementation.
constexpr int T_TOTAL = 0;
constexpr int T_RANK = 1;
constexpr int T_RCOMM = 2;
constexpr int T_VERIFY = 3;
constexpr int T_LAST = T_VERIFY;

int check_timer_flag_from_file();
void set_timer_enabled(int timeron);
bool timer_enabled();

void clear_timers();
void start_total_timer();
void stop_total_timer();
double read_total_timer();
double read_rank_timer();
void timer_start_if_enabled(int timer_id);
void timer_stop_if_enabled(int timer_id);
void print_timer_summary(int comm_size, const lci_irregular::IrregularRuntime& runtime);

} // namespace is_lci
