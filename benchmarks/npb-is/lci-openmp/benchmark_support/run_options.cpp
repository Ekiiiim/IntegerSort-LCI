#include "benchmark_support/run_options.hpp"

#ifdef _OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

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

int benchmark_thread_count() {
#ifdef _OPENMP
  return omp_get_max_threads();
#else
  return 1;
#endif
}

int read_threads_per_device_option() {
  const char* env_val = getenv("NUM_THREADS_PER_DEVICE");
  if (env_val == nullptr) {
    return 1;
  }

  return atoi(env_val);
}

std::string read_env_or_default(const char* name, const char* default_value) {
  const char* value = getenv(name);
  return value == nullptr || *value == '\0' ? default_value : value;
}

} // namespace

namespace is_lci {

int read_use_upacket_flag() {
  return read_bool_env_flag("USE_UPACKET", 1);
}

int read_loopback_flag() {
  return read_bool_env_flag("LOOPBACK", 1);
}

lci_irregular::RuntimeOptions read_lci_runtime_options() {
  lci_irregular::RuntimeOptions options;
  int threads_per_device = read_threads_per_device_option();
  if (threads_per_device <= 0) {
    threads_per_device = 1;
  }

  options.device_count = std::max(1, benchmark_thread_count() / threads_per_device);
#ifdef IS_LCI_ENABLE_TIMERS
  options.profiling.enabled = true;
  options.profiling.worker_count = static_cast<size_t>(benchmark_thread_count());
  options.profiling.output_directory = read_env_or_default("LCI_IRREGULAR_PROFILE_DIR", "profiles");
  options.profiling.file_prefix = "npb-is-lci";
#endif
  return options;
}

} // namespace is_lci
