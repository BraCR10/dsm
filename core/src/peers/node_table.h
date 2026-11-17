#ifndef DSM_PEERS_NODE_TABLE_H
#define DSM_PEERS_NODE_TABLE_H

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dsm/dsm.h"
#include "sync/mutex.h"

namespace dsm::peers {

using Clock = std::chrono::steady_clock;

struct NodeRecord {
    uint32_t nodeId = 0;
    std::string ip;
    uint16_t mainPort = 0;
    uint16_t heartbeatPort = 0;
    uint64_t sharedMemoryBytes = 0;
    uint64_t freeMemoryBytes = 0;
    uint64_t rangeStart = 0;
    bool active = false;
    Clock::time_point lastSeen{};
};

class NodeTable {
public:
    void upsert(const NodeRecord& record);
    bool markActive(uint32_t nodeId, uint64_t freeMemoryBytes, Clock::time_point now);
    bool markDown(uint32_t nodeId);
    bool isActive(uint32_t nodeId);
    bool isRecentlySeen(uint32_t nodeId, std::chrono::milliseconds window, Clock::time_point now);
    std::vector<uint32_t> activeNodeIds();
    std::vector<uint32_t> nodesWithFreeMemory(uint64_t requiredBytes, uint32_t requesterNodeId);
    std::vector<uint32_t> takeExpired(std::chrono::milliseconds timeout, Clock::time_point now, uint32_t selfNodeId);
    std::vector<NodeRecord> snapshot();
    std::vector<NodeInfo> toNodeInfo();

private:
    sync::Mutex mutex_;
    std::map<uint32_t, NodeRecord> records_;
};

}  // namespace dsm::peers

#endif
