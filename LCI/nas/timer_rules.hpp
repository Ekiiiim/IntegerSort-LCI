#pragma once

namespace is_lci {

// NAS IS timer IDs and output labels from the NPB MPI implementation.
constexpr int T_TOTAL = 0;
constexpr int T_RANK = 1;
constexpr int T_RCOMM = 2;
constexpr int T_VERIFY = 3;
constexpr int T_LAST = T_VERIFY;

const char* timer_label(int timer_id);

} // namespace is_lci
