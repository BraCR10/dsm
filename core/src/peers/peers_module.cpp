#include "peers/peers_module.h"

#include "common/log.h"
#include "peers/peer_messages.h"

namespace dsm::peers {

namespace {

constexpr const char* kModule = "peers";
constexpr uint32_t kJoinTimeoutMs = 1000;

using network::Channel;
using network::IncomingMessage;
using network::makeMessage;
using network::Message;
using network::MessageType;

}  // namespace

PeersModule::PeersModule(const settings::ClusterConfig& config, uint32_t selfNodeId, uint64_t sharedMemoryBytes,
                         network::NetworkModule& network, FreeMemoryProvider freeMemoryProvider)
    : config_(config),
      selfNodeId_(selfNodeId),
      sharedMemoryBytes_(sharedMemoryBytes),
      network_(network),
      freeMemoryProvider_(std::move(freeMemoryProvider)) {
    for (const settings::NodeConfig& node : config_.nodes) {
        nodeTable_.upsert(node.nodeId == selfNodeId_ ? selfRecord() : recordFromConfig(node));
        network_.registerPeerAddress(node.nodeId, node.ip, node.mainPort, node.heartbeatPort);
    }
    nodeTable_.markActive(selfNodeId_, freeMemoryProvider_(), Clock::now());
}

PeersModule::~PeersModule() {
    leaveCluster();
}

NodeRecord PeersModule::recordFromConfig(const settings::NodeConfig& node) const {
    NodeRecord record;
    record.nodeId = node.nodeId;
    record.ip = node.ip;
    record.mainPort = node.mainPort;
    record.heartbeatPort = node.heartbeatPort;
    record.sharedMemoryBytes = node.memorySize;
    record.freeMemoryBytes = node.memorySize;
    record.rangeStart = config_.baseAddress + static_cast<uint64_t>(node.nodeId) * config_.nodeRangeSpacing;
    return record;
}

NodeRecord PeersModule::selfRecord() const {
    NodeRecord record = recordFromConfig(*config_.findNode(selfNodeId_));
    record.sharedMemoryBytes = sharedMemoryBytes_;
    record.freeMemoryBytes = sharedMemoryBytes_;
    record.active = true;
    return record;
}

void PeersModule::registerHandlers() {
    network_.registerHandler(MessageType::NodeJoin, [this](const IncomingMessage& incoming) {
        handleNodeJoin(incoming);
    });
    network_.registerHandler(MessageType::Heartbeat, [this](const IncomingMessage& incoming) {
        handleHeartbeat(incoming);
    });
    network_.registerHandler(MessageType::NodeDown, [this](const IncomingMessage& incoming) {
        handleNodeDown(incoming);
    });
    network_.registerHandler(MessageType::NodeLeave, [this](const IncomingMessage& incoming) {
        handleNodeDown(incoming);
    });
}

DsmStatus PeersModule::joinCluster() {
    NodeJoinPayload joinPayload{selfRecord(), config_.configHash};
    for (const settings::NodeConfig& node : config_.nodes) {
        if (node.nodeId == selfNodeId_) {
            continue;
        }
        Message response;
        const DsmStatus status = network_.request(node.nodeId, Channel::Main,
                                                  makeMessage(MessageType::NodeJoin, encodeNodeJoin(joinPayload)),
                                                  kJoinTimeoutMs, response);
        if (status != DSM_OK) {
            continue;
        }
        if (response.header.status != DSM_OK) {
            common::logError(kModule, common::fromNode(node.nodeId),
                             "join rejected with status %d", response.header.status);
            return static_cast<DsmStatus>(response.header.status);
        }
        const auto nodes = decodeNodeList(response.payload);
        if (nodes) {
            handleNodeJoinAck(node.nodeId, *nodes);
        }
    }
    common::logInfo(kModule, common::fromSelf(),
                    "joined the cluster with %zu active nodes", nodeTable_.activeNodeIds().size());
    return DSM_OK;
}

void PeersModule::handleNodeJoinAck(uint32_t reporterNodeId, const std::vector<NodeRecord>& nodes) {
    for (const NodeRecord& node : nodes) {
        if (node.nodeId != selfNodeId_ && nodeTable_.markActive(node.nodeId, node.freeMemoryBytes, Clock::now())) {
            common::logInfo(kModule, common::fromNode(reporterNodeId), "node %u is active", node.nodeId);
        }
    }
}

void PeersModule::handleNodeJoin(const IncomingMessage& incoming) {
    const auto joinPayload = decodeNodeJoin(incoming.message.payload);
    Message response = makeMessage(MessageType::NodeJoinAck);
    if (!joinPayload) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
    } else if (joinPayload->configHash != config_.configHash) {
        response.header.status = DSM_ERR_CONFIG_MISMATCH;
    } else if (joinPayload->node.nodeId == selfNodeId_ || config_.findNode(joinPayload->node.nodeId) == nullptr ||
               nodeTable_.isRecentlySeen(joinPayload->node.nodeId,
                                         std::chrono::seconds(config_.heartbeatTimeoutSec), Clock::now())) {
        response.header.status = DSM_ERR_DUPLICATE_NODE;
    }
    if (response.header.status != DSM_OK) {
        common::logError(kModule, common::fromNode(incoming.message.header.sourceNode),
                         "rejected its join with status %d",
                         response.header.status);
        network_.reply(incoming, std::move(response));
        return;
    }
    NodeRecord record = joinPayload->node;
    record.active = false;
    nodeTable_.upsert(record);
    nodeTable_.markActive(record.nodeId, record.freeMemoryBytes, Clock::now());
    common::logInfo(kModule, common::fromNode(record.nodeId), "node %u joined the cluster", record.nodeId);

    std::vector<NodeRecord> activeNodes;
    for (const NodeRecord& node : nodeTable_.snapshot()) {
        if (node.active) {
            activeNodes.push_back(node);
        }
    }
    response.payload = encodeNodeList(activeNodes);
    network_.reply(incoming, std::move(response));
}

void PeersModule::handleHeartbeat(const IncomingMessage& incoming) {
    const auto heartbeat = decodeHeartbeat(incoming.message.payload);
    if (heartbeat && heartbeat->nodeId != selfNodeId_ &&
        nodeTable_.markActive(heartbeat->nodeId, heartbeat->freeMemoryBytes, Clock::now())) {
        common::logInfo(kModule, common::fromNode(heartbeat->nodeId), "node %u is active", heartbeat->nodeId);
    }
}

void PeersModule::handleNodeDown(const IncomingMessage& incoming) {
    const auto nodeId = decodeNodeId(incoming.message.payload);
    if (!nodeId || *nodeId == selfNodeId_) {
        return;
    }
    const uint32_t reporter = incoming.message.header.sourceNode;
    const char* messageName = network::messageTypeName(incoming.message.type());
    const bool reportedByAnotherNode = incoming.message.type() == MessageType::NodeDown;
    if (reportedByAnotherNode &&
        nodeTable_.isRecentlySeen(*nodeId, std::chrono::seconds(config_.heartbeatTimeoutSec), Clock::now())) {
        common::logInfo(kModule, common::fromNode(reporter),
                        "ignoring %s for node %u because its heartbeats still arrive",
                        messageName, *nodeId);
        return;
    }
    if (nodeTable_.markDown(*nodeId)) {
        network_.closeConnectionsTo(*nodeId);
        common::logInfo(kModule, common::fromNode(reporter), "node %u left the cluster (%s)", *nodeId, messageName);
    }
}

void PeersModule::startHeartbeat() {
    if (heartbeatRunning_.exchange(true)) {
        return;
    }
    heartbeatThread_ = std::thread(&PeersModule::heartbeatLoop, this);
}

void PeersModule::heartbeatLoop() {
    const auto timeout = std::chrono::seconds(config_.heartbeatTimeoutSec);
    while (heartbeatRunning_.load()) {
        const HeartbeatPayload payload{selfNodeId_, freeMemoryProvider_()};
        nodeTable_.markActive(selfNodeId_, payload.freeMemoryBytes, Clock::now());
        for (uint32_t activeNodeId : nodeTable_.activeNodeIds()) {
            if (activeNodeId != selfNodeId_) {
                network_.send(activeNodeId, Channel::Heartbeat,
                              makeMessage(MessageType::Heartbeat, encodeHeartbeat(payload)));
            }
        }
        for (uint32_t expiredNodeId : nodeTable_.takeExpired(timeout, Clock::now(), selfNodeId_)) {
            declareNodeDown(expiredNodeId);
        }
        heartbeatStop_.waitFor(config_.heartbeatIntervalMs);
    }
}

void PeersModule::declareNodeDown(uint32_t nodeId) {
    common::logError(kModule, common::fromSelf(), "node %u missed its heartbeats and is considered down", nodeId);
    network_.closeConnectionsTo(nodeId);
    for (uint32_t activeNodeId : nodeTable_.activeNodeIds()) {
        if (activeNodeId != selfNodeId_) {
            network_.send(activeNodeId, Channel::Main, makeMessage(MessageType::NodeDown, encodeNodeId(nodeId)));
        }
    }
}

void PeersModule::leaveCluster() {
    if (!heartbeatRunning_.exchange(false)) {
        return;
    }
    heartbeatStop_.post();
    heartbeatThread_.join();
    for (uint32_t activeNodeId : nodeTable_.activeNodeIds()) {
        if (activeNodeId != selfNodeId_) {
            network_.send(activeNodeId, Channel::Main, makeMessage(MessageType::NodeLeave, encodeNodeId(selfNodeId_)));
        }
    }
}

}  // namespace dsm::peers
