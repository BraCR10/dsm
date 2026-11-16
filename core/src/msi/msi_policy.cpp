#include "msi/msi_policy.h"

namespace dsm::msi {

using network::AccessType;

namespace {

bool stateAllows(MsiState state, AccessType access) {
    return access == AccessType::Read ? state != MSI_INVALID : state == MSI_MODIFIED;
}

}  // namespace

LocalFaultAction decideLocalFault(bool isHomeNode, bool isAllocated, MsiState localState, AccessType access) {
    if (!isHomeNode) {
        return stateAllows(localState, access) ? LocalFaultAction::GrantLocally : LocalFaultAction::FetchFromHome;
    }
    if (!isAllocated) {
        return LocalFaultAction::RejectNotAllocated;
    }
    if (localState == MSI_INVALID) {
        return LocalFaultAction::RecallFromHolderThenGrant;
    }
    if (!stateAllows(localState, access)) {
        return LocalFaultAction::InvalidateCopiesThenGrantWrite;
    }
    return LocalFaultAction::GrantLocally;
}

RemoteRequestAction decideRemoteRequest(bool isAllocated, bool anotherNodeHoldsModified, AccessType access) {
    if (!isAllocated) {
        return RemoteRequestAction::RejectNotAllocated;
    }
    if (access == AccessType::Read) {
        return anotherNodeHoldsModified ? RemoteRequestAction::RecallThenShare : RemoteRequestAction::ShareCopy;
    }
    return anotherNodeHoldsModified ? RemoteRequestAction::RecallThenGrantExclusive
                                    : RemoteRequestAction::GrantExclusive;
}

MsiState stateAfterRecall(AccessType requestedAccess) {
    return requestedAccess == AccessType::Read ? MSI_SHARED : MSI_INVALID;
}

const char* msiStateName(MsiState state) {
    switch (state) {
        case MSI_MODIFIED: return "M";
        case MSI_SHARED: return "S";
        case MSI_INVALID: return "I";
    }
    return "?";
}

}  // namespace dsm::msi
