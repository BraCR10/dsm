#ifndef DSM_STATS_STATS_MODULE_H
#define DSM_STATS_STATS_MODULE_H

#include <vector>

#include "dsm/dsm.h"
#include "memory/memory_module.h"
#include "network/network_module.h"

namespace dsm::stats {

class StatsModule {
public:
    StatsModule(network::NetworkModule& network, memory::MemoryModule& memory);

    void registerHandlers();
    DsmStats localStats();
    DsmStatus remoteStats(uint32_t nodeId, DsmStats& outStats);
    DsmStatus remotePageTable(uint32_t nodeId, std::vector<PageTableEntry>& outEntries);

private:
    network::NetworkModule& network_;
    memory::MemoryModule& memory_;
};

}  // namespace dsm::stats

#endif
