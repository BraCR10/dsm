#ifndef DSM_MEMORY_MEMORY_MODULE_H
#define DSM_MEMORY_MEMORY_MODULE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "dsm/dsm.h"
#include "memory/address_map.h"
#include "memory/address_registry.h"
#include "memory/page_access_controller.h"
#include "memory/shared_window.h"
#include "msi/msi_module.h"
#include "msi/page_messages.h"
#include "network/message_worker.h"
#include "network/network_module.h"
#include "settings/cluster_config.h"

namespace dsm::memory {

struct MemoryCounters {
    std::atomic<uint64_t> pageFaults{0};
    std::atomic<uint64_t> pagesSent{0};
    std::atomic<uint64_t> pagesReceived{0};
    std::atomic<uint64_t> invalidations{0};
};

struct FaultResolution {
    DsmStatus status = DSM_OK;
    uint64_t pageId = 0;
    int protection = 0;
};

using FaultReply = std::function<void(const FaultResolution&)>;
using RemoteMemoryCandidates = std::function<std::vector<uint32_t>(uint64_t requiredBytes)>;

class MemoryModule {
public:
    MemoryModule(const settings::ClusterConfig& config, uint32_t selfNodeId, uint64_t sharedMemoryBytes,
                 network::NetworkModule& network, msi::MsiModule& msi);
    MemoryModule(const MemoryModule&) = delete;
    MemoryModule& operator=(const MemoryModule&) = delete;

    DsmStatus start();
    void stop();
    void registerHandlers();
    void setAccessController(PageAccessController* controller) { accessController_ = controller; }
    void setRemoteMemoryCandidates(RemoteMemoryCandidates candidates) { remoteMemoryCandidates_ = std::move(candidates); }

    uintptr_t allocate(size_t size, uint32_t processId, DsmStatus& outStatus);
    DsmStatus release(uintptr_t address, uint64_t& outPageCount);
    void resolveApplicationFault(uintptr_t address, bool isWrite, const FaultReply& replyWhilePageIsLocked);
    DsmStatus pageState(uintptr_t address, MsiState& outState);
    std::vector<PageTableEntry> pageTableView();

    uint64_t usedBytes();
    uint64_t freeBytes();
    const MemoryCounters& counters() const { return counters_; }

private:
    uintptr_t allocateLocally(uint64_t pageCount, uint32_t processId, DsmStatus& outStatus);
    uintptr_t allocateOnAnotherNode(uint64_t pageCount, uint32_t processId, DsmStatus& outStatus);
    DsmStatus releaseLocally(uintptr_t address, uint64_t& outPageCount);
    DsmStatus releaseOnHome(uint32_t homeNode, uintptr_t address, uint64_t& outPageCount);
    void resolveHomeFault(uint64_t pageId, network::AccessType access, const FaultReply& replyWhilePageIsLocked);
    void resolveRemoteFault(uint64_t pageId, uint32_t homeNode, network::AccessType access,
                            const FaultReply& replyWhilePageIsLocked);
    DsmStatus requestPageFromHome(uint32_t homeNode, uint64_t pageId, network::AccessType access,
                                  msi::PageDataPayload& outPage);
    DsmStatus recallFromHolder(msi::PageTable::Operation& operation, uint64_t pageId, network::AccessType access);
    void invalidateNodes(msi::PageTable::Operation& operation, uint64_t pageId, const std::set<uint32_t>& nodes);
    uint64_t nextVersion(msi::PageTable::Operation& operation);
    network::Message pageDataMessage(uint64_t pageId, MsiState grantedState, uint64_t version);
    void restrictApplications(uint64_t pageId, int protection);
    uint8_t* pageData(uint64_t pageId) const;
    uint64_t localPageIndex(uint64_t pageId) const;

    void handleWaitingRequest(const network::IncomingMessage& incoming);
    void handlePageRequest(const network::IncomingMessage& incoming);
    void handleMemoryRequest(const network::IncomingMessage& incoming);
    void handleMemoryFree(const network::IncomingMessage& incoming);
    void handlePageRecall(const network::IncomingMessage& incoming);
    void handleInvalidate(const network::IncomingMessage& incoming);

    const settings::ClusterConfig& config_;
    const uint32_t selfNodeId_;
    const AddressMap addressMap_;
    const uint64_t firstLocalPageId_;
    network::NetworkModule& network_;
    msi::MsiModule& msi_;
    AddressRegistry registry_;
    MemoryCounters counters_;
    SharedWindow window_;
    PageAccessController* accessController_ = nullptr;
    RemoteMemoryCandidates remoteMemoryCandidates_;
    network::MessageWorker waitingRequestWorker_;
};

}  // namespace dsm::memory

#endif
