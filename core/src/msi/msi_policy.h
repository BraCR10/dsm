#ifndef DSM_MSI_MSI_POLICY_H
#define DSM_MSI_MSI_POLICY_H

#include "dsm/dsm.h"
#include "network/message.h"

namespace dsm::msi {

enum class LocalFaultAction {
    GrantLocally,
    FetchFromHome,
    InvalidateCopiesThenGrantWrite,
    RecallFromHolderThenGrant,
    RejectNotAllocated,
};

enum class RemoteRequestAction {
    ShareCopy,
    RecallThenShare,
    GrantExclusive,
    RecallThenGrantExclusive,
    RequesterAlreadyHolds,
    RejectNotAllocated,
};

LocalFaultAction decideLocalFault(bool isHomeNode, bool isAllocated, MsiState localState,
                                  network::AccessType access);

RemoteRequestAction decideRemoteRequest(bool isAllocated, bool anotherNodeHoldsModified, network::AccessType access,
                                        bool requesterHoldsModified = false);

MsiState stateAfterRecall(network::AccessType requestedAccess);

const char* msiStateName(MsiState state);

}  // namespace dsm::msi

#endif
