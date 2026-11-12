#ifndef DSM_PEERS_PEERS_MODULE_H
#define DSM_PEERS_PEERS_MODULE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "dsm/dsm.h"
#include "network/network_module.h"
#include "peers/node_table.h"
#include "settings/cluster_config.h"
#include "sync/semaphore.h"

namespace dsm::peers {

using FreeMemoryProvider = std::function<uint64_t()>;

class PeersModule {
public:
    PeersModule(const settings::ClusterConfig& config, uint32_t selfNodeId, uint64_t sharedMemoryBytes,
                network::NetworkModule& network, FreeMemoryProvider freeMemoryProvider);
    ~PeersModule();
    PeersModule(const PeersModule&) = delete;
    PeersModule& operator=(const PeersModule&) = delete;

    void registerHandlers();
    DsmStatus joinCluster();
    void startHeartbeat();
    void leaveCluster();

    bool isActive(uint32_t nodeId) { return nodeTable_.isActive(nodeId); }
    NodeTable& nodeTable() { return nodeTable_; }

private:
    NodeRecord selfRecord() const;
    NodeRecord recordFromConfig(const settings::NodeConfig& node) const;
    void handleNodeJoin(const network::IncomingMessage& incoming);
    void handleNodeJoinAck(const std::vector<NodeRecord>& nodes);
    void handleHeartbeat(const network::IncomingMessage& incoming);
    void handleNodeDown(const network::IncomingMessage& incoming);
    void heartbeatLoop();
    void declareNodeDown(uint32_t nodeId);

    const settings::ClusterConfig& config_;
    const uint32_t selfNodeId_;
    const uint64_t sharedMemoryBytes_;
    network::NetworkModule& network_;
    FreeMemoryProvider freeMemoryProvider_;
    NodeTable nodeTable_;
    std::atomic<bool> heartbeatRunning_{false};
    sync::Semaphore heartbeatStop_;
    std::thread heartbeatThread_;
};

}  // namespace dsm::peers

#endif
