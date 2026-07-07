#pragma once

#include "benchmark/nas/problem_config.hpp"

namespace is_lci {

// NAS INT_TYPE used as a key value in key arrays/messages.
using KeyValue = int;

// NAS INT_TYPE used as a local bucket/histogram/receive count.
using KeyCount = int;

#if CLASS == 'D' || CLASS == 'E'
// NAS KEY_TYPE for global key ranks; large classes exceed 32-bit range.
using KeyRank = long;
#else
// NAS KEY_TYPE for global key ranks in smaller classes.
using KeyRank = int;
#endif

} // namespace is_lci
