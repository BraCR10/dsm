#include "api/node_runtime.h"

#include "common/log.h"

namespace dsm::api {

NodeRuntime::NodeRuntime(settings::ClusterConfig config, uint32_t selfNodeId, uint64_t sharedMemoryBytes)
    : config_(std::move(config)), selfNodeId_(selfNodeId) {
    network_ = std::make_unique<network::NetworkModule>(selfNodeId_);
    msi_ = std::make_unique<msi::MsiModule>();
    memory_ = std::make_unique<memory::MemoryModule>(config_, selfNodeId_, sharedMemoryBytes, *network_, *msi_);
    peers_ = std::make_unique<peers::PeersModule>(config_, selfNodeId_, sharedMemoryBytes, *network_,
                                                  [this] { return memory_->freeBytes(); });
    stats_ = std::make_unique<stats::StatsModule>(*network_, *memory_);
}

NodeRuntime::~NodeRuntime() {
    shutdown();
}

DsmStatus NodeRuntime::start() {
    common::setLogNodeId(selfNodeId_);
    peers_->registerHandlers();
    memory_->registerHandlers();
    stats_->registerHandlers();

    DsmStatus status = memory_->start();
    if (status != DSM_OK) {
        return status;
    }
    const settings::NodeConfig* self = config_.findNode(selfNodeId_);
    status = network_->start(self->mainPort, self->heartbeatPort);
    if (status != DSM_OK) {
        memory_->stop();
        return status;
    }
    status = peers_->joinCluster();
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
    peers_->leaveCluster();
    network_->stop();
    memory_->stop();
    common::logInfo("api", "node %u finalized", selfNodeId_);
}

}  // namespace dsm::api
