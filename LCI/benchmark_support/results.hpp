#pragma once

namespace is_lci {

void print_initial_status(int my_rank, int np_total, int comm_size, int use_upacket, int use_loopback);
void print_final_results(int comm_size, int np_total, double maxtime, int passed_verification);

} // namespace is_lci
