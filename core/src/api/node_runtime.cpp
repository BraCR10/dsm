#include "api/node_runtime.h"

#include "common/log.h"
#include "network/unix_socket.h"

namespace dsm::api {

std::optional<uint32_t> detectLocalNodeId(const settings::ClusterConfig& config) {
    for (const settings::NodeConfig& node : config.nodes) {
        if (network::isLocalAddress(node.ip) && network::isTcpPortAvailable(node.mainPort) &&
            network::isTcpPortAvailable(node.heartbeatPort)) {
            return node.nodeId;
        }
    }
    return std::nullopt;
}

NodeRuntime::NodeRuntime(settings::ClusterConfig config, uint32_t selfNodeId, uint64_t sharedMemoryBytes)
    : config_(std::move(config)), selfNodeId_(selfNodeId) {
    network_ = std::make_unique<network::NetworkModule>(selfNodeId_);
    msi_ = std::make_unique<msi::MsiModule>();
    memory_ = std::make_unique<memory::MemoryModule>(config_, selfNodeId_, sharedMemoryBytes, *network_, *msi_);
    peers_ = std::make_unique<peers::PeersModule>(config_, selfNodeId_, sharedMemoryBytes, *network_,
                                                  [this] { return memory_->freeBytes(); });
    stats_ = std::make_unique<stats::StatsModule>(*network_, *memory_);
    sync_ = std::make_unique<sync::DistributedSyncModule>(selfNodeId_, *network_);
    localService_ = std::make_unique<LocalService>(config_, selfNodeId_, *memory_, *stats_, *peers_, *sync_);
    memory_->setAccessController(localService_.get());
    peers_->setNodeDownListener([this](uint32_t nodeId) { sync_->releaseNode(nodeId); });
    memory_->setRemoteMemoryCandidates([this](uint64_t requiredBytes) {
        return peers_->nodeTable().nodesWithFreeMemory(requiredBytes, selfNodeId_);
    });
}

NodeRuntime::~NodeRuntime() {
    shutdown();
}

DsmStatus NodeRuntime::start() {
    common::setLogNodeId(selfNodeId_);
    peers_->registerHandlers();
    memory_->registerHandlers();
    stats_->registerHandlers();
    sync_->registerHandlers();

    DsmStatus status = memory_->start();
    if (status != DSM_OK) {
        return status;
    }
    const settings::NodeConfig* self = config_.findNode(selfNodeId_);
    status = network_->start(self->mainPort, self->heartbeatPort);
    if (status == DSM_OK) {
        status = peers_->joinCluster();
    }
    if (status == DSM_OK) {
        status = localService_->start();
    }
    if (status != DSM_OK) {
        network_->stop();
        memory_->stop();
        return status;
    }
    peers_->startHeartbeat();
    started_ = true;
    return DSM_OK;
}

void NodeRuntime::shutdown() {
    if (!started_) {
        return;
    }
    started_ = false;
    localService_->stop();
    peers_->leaveCluster();
    network_->stop();
    memory_->stop();
    common::logInfo("api", common::fromSelf(), "node stopped");
}

}  // namespace dsm::api
