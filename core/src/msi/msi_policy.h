#ifndef DSM_MSI_MSI_POLICY_H
#define DSM_MSI_MSI_POLICY_H

#include "dsm/dsm.h"
#include "network/message.h"

namespace dsm::msi {

enum class LocalFaultAction {
    GrantLocally,
    FetchReadFromHome,
    InvalidateCopiesThenGrantWrite,
    RejectNotAllocated,
    RejectRemoteWrite,
};

enum class RemoteRequestAction {
    ShareCopy,
    RejectNotAllocated,
    RejectRemoteWrite,
};

LocalFaultAction decideLocalFault(bool isHomeNode, bool isAllocated, MsiState localState,
                                  network::AccessType access);

RemoteRequestAction decideRemoteRequest(bool isAllocated, network::AccessType access);

const char* msiStateName(MsiState state);

}  // namespace dsm::msi

#endif
