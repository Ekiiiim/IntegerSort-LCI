#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace lci_irregular {
namespace detail {

template <typename Record> size_t message_bytes(size_t batch_records) {
  return batch_records * sizeof(Record);
}

template <typename Record> size_t record_count_from_message_bytes(size_t bytes) {
  if (bytes == 0 || bytes % sizeof(Record) != 0) {
    std::fprintf(stderr, "LCI irregular AM payload size %zu is not a valid record batch\n", bytes);
    std::abort();
  }
  return bytes / sizeof(Record);
}

} // namespace detail
} // namespace lci_irregular
