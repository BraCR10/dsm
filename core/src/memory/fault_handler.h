#ifndef DSM_MEMORY_FAULT_HANDLER_H
#define DSM_MEMORY_FAULT_HANDLER_H

#include <cstdint>

#include "memory/pending_faults.h"

namespace dsm::memory {

bool installFaultHandler(uintptr_t windowStart, uint64_t windowSize, PendingFaults& pendingFaults);
void uninstallFaultHandler();

}  // namespace dsm::memory

#endif
