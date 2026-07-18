#pragma once

#include <lci.hpp>

#include <cstddef>
#include <cstdint>

namespace lci_irregular {
class IrregularRuntime;

namespace detail {

struct AmMessageHeader {
  uint32_t exchange_id;
  uint32_t record_count;
};

class AmExchangeStateBase {
public:
  virtual ~AmExchangeStateBase() = default;
  virtual void receive_message(int source_rank, const void* message, size_t message_bytes) = 0;
};

IrregularRuntime& active_runtime();
void am_handler(lci::status_t status);

} // namespace detail
} // namespace lci_irregular
