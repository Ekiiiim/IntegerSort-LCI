#pragma once

namespace is_lci {

void print_initial_status(int my_rank, int total_rank_count, int active_rank_count, int use_upacket, int use_loopback);
void print_final_results(int active_rank_count, int total_rank_count, double maxtime, int passed_verification);

} // namespace is_lci
