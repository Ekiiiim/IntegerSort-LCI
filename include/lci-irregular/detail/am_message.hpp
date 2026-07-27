#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lci_irregular {
namespace detail {

struct AmMessageHeader {
  uint32_t record_count;
};

inline size_t align_up(size_t value, size_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

template <typename Record> size_t header_bytes() {
  return align_up(sizeof(AmMessageHeader), alignof(Record));
}

template <typename Record> size_t message_bytes(size_t batch_records) {
  return header_bytes<Record>() + batch_records * sizeof(Record);
}

template <typename Record> void write_message_header(void* buffer, size_t record_count) {
  if (record_count > std::numeric_limits<uint32_t>::max()) {
    std::fprintf(stderr, "LCI irregular AM batch has too many records: %zu\n", record_count);
    std::abort();
  }

  AmMessageHeader header{static_cast<uint32_t>(record_count)};
  std::memcpy(buffer, &header, sizeof(header));
}

} // namespace detail
} // namespace lci_irregular
