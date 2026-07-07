#include "benchmark/driver/options.hpp"

#include <cstdlib>
#include <cstring>

namespace {

int read_bool_env_flag(const char* name, int default_value) {
  int flag = default_value;
  char* ev = getenv(name);

  if (ev) {
    if (*ev == '\0') {
      flag = 1;
    } else if (*ev >= '1' && *ev <= '9') {
      flag = 1;
    } else if (strcmp(ev, "on") == 0 || strcmp(ev, "ON") == 0 || strcmp(ev, "yes") == 0 || strcmp(ev, "YES") == 0 ||
               strcmp(ev, "true") == 0 || strcmp(ev, "TRUE") == 0) {
      flag = 1;
    } else if (strcmp(ev, "off") == 0 || strcmp(ev, "OFF") == 0 || strcmp(ev, "no") == 0 || strcmp(ev, "NO") == 0 ||
               strcmp(ev, "false") == 0 || strcmp(ev, "FALSE") == 0 || strcmp(ev, "0") == 0) {
      flag = 0;
    }
  }

  return flag;
}

} // namespace

namespace is_lci {

int read_use_upacket_flag() {
  return read_bool_env_flag("USE_UPACKET", 1);
}

int read_loopback_flag() {
  return read_bool_env_flag("LOOPBACK", 1);
}

} // namespace is_lci
