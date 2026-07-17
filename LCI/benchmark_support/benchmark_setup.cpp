#include "benchmark_support/benchmark_setup.hpp"

#include <cstdio>

namespace is_lci {

void configure_benchmark_output() {
  setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace is_lci
