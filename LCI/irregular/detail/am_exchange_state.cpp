#include "irregular/detail/am_exchange_state.hpp"

#include "irregular/irregular_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lci_irregular {
namespace detail {

void am_handler(lci::status_t status) {
  if (status.get_size() < sizeof(AmMessageHeader)) {
    std::fprintf(stderr, "Received malformed LCI irregular AM payload: %zu bytes\n", status.get_size());
    std::abort();
  }

  AmMessageHeader header{};
  std::memcpy(&header, status.get_buffer(), sizeof(header));
  AmExchangeStateBase* state = find_exchange(active_runtime(), header.exchange_id);
  if (state == nullptr) {
    std::fprintf(stderr, "Received LCI irregular AM for inactive exchange id %u\n", header.exchange_id);
    std::abort();
  }

  state->receive_message(status.get_rank(), status.get_buffer(), status.get_size());
  lci::put_upacket(status.get_buffer());
}

} // namespace detail
} // namespace lci_irregular
