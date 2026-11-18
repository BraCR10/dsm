#ifndef DSM_MSI_MSI_MODULE_H
#define DSM_MSI_MSI_MODULE_H

#include "msi/msi_policy.h"
#include "msi/page_table.h"

namespace dsm::msi {

class MsiModule {
public:
    PageTable& pageTable() { return pageTable_; }

    LocalFaultAction onLocalFault(const PageEntry& entry, uint32_t selfNodeId, network::AccessType access) const {
        return decideLocalFault(entry.homeNode == selfNodeId, entry.allocated, entry.state, access);
    }

    RemoteRequestAction onRemoteRequest(const PageEntry& entry, uint32_t selfNodeId, uint32_t requester,
                                        network::AccessType access) const {
        const bool anotherNodeHoldsModified = entry.allocated && entry.currentHolder != selfNodeId;
        return decideRemoteRequest(entry.allocated, anotherNodeHoldsModified, access,
                                   entry.currentHolder == requester);
    }

private:
    PageTable pageTable_;
};

}  // namespace dsm::msi

#endif
