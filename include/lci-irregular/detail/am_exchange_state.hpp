#pragma once

#include <lci.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

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
  virtual void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback,
                               std::optional<size_t> worker_index) noexcept = 0;
};

IrregularRuntime& active_runtime();
void dispatch_am_message(lci::status_t status) noexcept;

} // namespace detail
} // namespace lci_irregular
