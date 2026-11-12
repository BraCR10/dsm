#include "msi/msi_policy.h"

namespace dsm::msi {

using network::AccessType;

LocalFaultAction decideLocalFault(bool isHomeNode, bool isAllocated, MsiState localState, AccessType access) {
    if (!isHomeNode) {
        if (access == AccessType::Write) {
            return LocalFaultAction::RejectRemoteWrite;
        }
        return localState == MSI_INVALID ? LocalFaultAction::FetchReadFromHome : LocalFaultAction::GrantLocally;
    }
    if (!isAllocated) {
        return LocalFaultAction::RejectNotAllocated;
    }
    if (access == AccessType::Write && localState != MSI_MODIFIED) {
        return LocalFaultAction::InvalidateCopiesThenGrantWrite;
    }
    return LocalFaultAction::GrantLocally;
}

RemoteRequestAction decideRemoteRequest(bool isAllocated, AccessType access) {
    if (!isAllocated) {
        return RemoteRequestAction::RejectNotAllocated;
    }
    if (access == AccessType::Write) {
        return RemoteRequestAction::RejectRemoteWrite;
    }
    return RemoteRequestAction::ShareCopy;
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
