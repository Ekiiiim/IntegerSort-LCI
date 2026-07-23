#include <lci-irregular/detail/am_exchange_state.hpp>
#include <lci-irregular/detail/profiling.hpp>

#include <lci-irregular/irregular_runtime.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace lci_irregular {
namespace detail {

void dispatch_am_message(lci::status_t status) noexcept {
  try {
    if (status.get_size() < sizeof(AmMessageHeader)) {
      std::fprintf(stderr, "Received malformed LCI irregular AM payload: %zu bytes\n", status.get_size());
      std::abort();
    }

    AmMessageHeader header{};
    std::memcpy(&header, status.get_buffer(), sizeof(header));
    IrregularRuntime& runtime = active_runtime();
    AmExchangeStateBase* state = find_exchange(runtime, header.exchange_id);
    if (state == nullptr) {
      std::fprintf(stderr, "Received LCI irregular AM for inactive exchange id %u\n", header.exchange_id);
      std::abort();
    }

    std::optional<size_t> worker_index;
    if (runtime.profiling_enabled()) {
      worker_index = progress_worker();
    }
    state->receive_message(status.get_rank(), status.get_buffer(), status.get_size(), false, worker_index);
    lci::put_upacket(status.get_buffer());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LCI irregular AM handler terminated after exception: %s\n", error.what());
    std::terminate();
  } catch (...) {
    std::fprintf(stderr, "LCI irregular AM handler terminated after unknown exception\n");
    std::terminate();
  }
}

} // namespace detail
} // namespace lci_irregular
