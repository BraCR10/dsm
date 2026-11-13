#ifndef DSM_API_NODE_RUNTIME_H
#define DSM_API_NODE_RUNTIME_H

#include <cstdint>
#include <memory>

#include "dsm/dsm.h"
#include "memory/memory_module.h"
#include "msi/msi_module.h"
#include "network/network_module.h"
#include "peers/peers_module.h"
#include "settings/cluster_config.h"
#include "stats/stats_module.h"

namespace dsm::api {

class NodeRuntime {
public:
    NodeRuntime(settings::ClusterConfig config, uint32_t selfNodeId, uint64_t sharedMemoryBytes);
    ~NodeRuntime();
    NodeRuntime(const NodeRuntime&) = delete;
    NodeRuntime& operator=(const NodeRuntime&) = delete;

    DsmStatus start();
    void shutdown();

    uint32_t selfNodeId() const { return selfNodeId_; }
    memory::MemoryModule& memory() { return *memory_; }
    stats::StatsModule& stats() { return *stats_; }

private:
    const settings::ClusterConfig config_;
    const uint32_t selfNodeId_;
    std::unique_ptr<network::NetworkModule> network_;
    std::unique_ptr<msi::MsiModule> msi_;
    std::unique_ptr<memory::MemoryModule> memory_;
    std::unique_ptr<peers::PeersModule> peers_;
    std::unique_ptr<stats::StatsModule> stats_;
    bool started_ = false;
};

}  // namespace dsm::api

#endif
