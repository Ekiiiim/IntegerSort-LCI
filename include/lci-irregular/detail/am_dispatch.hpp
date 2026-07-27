#pragma once

#include <lci-irregular/detail/am_message.hpp>

#include <lci.hpp>

#include <optional>

namespace lci_irregular {
class IrregularRuntime;

namespace detail {

class AmRuntimeStateBase;

class AmStateBase {
public:
  virtual ~AmStateBase() = default;
  virtual void receive_message(int source_rank, const void* message, size_t message_bytes, bool loopback,
                               std::optional<size_t> worker_index) noexcept = 0;
};

IrregularRuntime& active_runtime();
void dispatch_am_message(lci::status_t status) noexcept;
AmRuntimeStateBase& active_am_state(IrregularRuntime& runtime);

} // namespace detail
} // namespace lci_irregular
