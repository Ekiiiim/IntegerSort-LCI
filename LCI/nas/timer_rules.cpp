#include "nas/timer_rules.hpp"

namespace is_lci {

const char* timer_label(int timer_id) {
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

} // namespace is_lci
