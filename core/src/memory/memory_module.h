#ifndef DSM_MEMORY_MEMORY_MODULE_H
#define DSM_MEMORY_MEMORY_MODULE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "dsm/dsm.h"
#include "memory/address_map.h"
#include "memory/address_registry.h"
#include "memory/pending_faults.h"
#include "msi/msi_module.h"
#include "network/network_module.h"
#include "settings/cluster_config.h"

namespace dsm::memory {

struct MemoryCounters {
    std::atomic<uint64_t> pageFaults{0};
    std::atomic<uint64_t> pagesSent{0};
    std::atomic<uint64_t> pagesReceived{0};
    std::atomic<uint64_t> invalidations{0};
};

class MemoryModule {
public:
    MemoryModule(const settings::ClusterConfig& config, uint32_t selfNodeId, uint64_t sharedMemoryBytes,
                 network::NetworkModule& network, msi::MsiModule& msi);
    ~MemoryModule();
    MemoryModule(const MemoryModule&) = delete;
    MemoryModule& operator=(const MemoryModule&) = delete;

    DsmStatus start();
    void stop();
    void registerHandlers();

    void* allocate(size_t size, DsmStatus& outStatus);
    DsmStatus release(void* pointer);
    DsmStatus pageState(const void* address, MsiState& outState);
    std::vector<PageTableEntry> pageTableView();

    uint64_t usedBytes();
    uint64_t freeBytes();
    const MemoryCounters& counters() const { return counters_; }

private:
    void resolverLoop();
    void resolveFault(const FaultRequest& fault);
    void fetchSharedCopy(msi::PageTable::Operation& operation, uint64_t pageId);
    void invalidateCopies(msi::PageTable::Operation& operation, uint64_t pageId);
    void protectPage(uint64_t pageId, int protection);
    void* pagePointer(uint64_t pageId) const;
    uint64_t localPageIndex(uint64_t pageId) const;

    void handlePageRequest(const network::IncomingMessage& incoming);
    void handleInvalidate(const network::IncomingMessage& incoming);

    const settings::ClusterConfig& config_;
    const uint32_t selfNodeId_;
    const AddressMap addressMap_;
    const uint64_t firstLocalPageId_;
    network::NetworkModule& network_;
    msi::MsiModule& msi_;
    AddressRegistry registry_;
    PendingFaults pendingFaults_;
    MemoryCounters counters_;
    void* window_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread resolverThread_;
};

}  // namespace dsm::memory

#endif
