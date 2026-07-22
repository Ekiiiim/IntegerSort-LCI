#pragma once

#include <lci-irregular/irregular_runtime.hpp>

namespace is_lci {

int read_use_upacket_flag();
int read_loopback_flag();
lci_irregular::IrregularRuntimeOptions read_lci_runtime_options();

} // namespace is_lci
