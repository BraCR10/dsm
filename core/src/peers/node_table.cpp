#include "peers/node_table.h"

#include <algorithm>
#include <cstring>

namespace dsm::peers {

void NodeTable::upsert(const NodeRecord& record) {
    sync::LockGuard guard(mutex_);
    records_[record.nodeId] = record;
}

bool NodeTable::markActive(uint32_t nodeId, uint64_t freeMemoryBytes, Clock::time_point now) {
    sync::LockGuard guard(mutex_);
    const auto found = records_.find(nodeId);
    if (found == records_.end()) {
        return false;
    }
    const bool wasActive = found->second.active;
    found->second.active = true;
    found->second.freeMemoryBytes = freeMemoryBytes;
    found->second.lastSeen = now;
    return !wasActive;
}

bool NodeTable::markDown(uint32_t nodeId) {
    sync::LockGuard guard(mutex_);
    const auto found = records_.find(nodeId);
    if (found == records_.end() || !found->second.active) {
        return false;
    }
    found->second.active = false;
    return true;
}

bool NodeTable::isActive(uint32_t nodeId) {
    sync::LockGuard guard(mutex_);
    const auto found = records_.find(nodeId);
    return found != records_.end() && found->second.active;
}

bool NodeTable::isRecentlySeen(uint32_t nodeId, std::chrono::milliseconds window, Clock::time_point now) {
    sync::LockGuard guard(mutex_);
    const auto found = records_.find(nodeId);
    return found != records_.end() && found->second.active && now - found->second.lastSeen < window;
}

std::vector<uint32_t> NodeTable::activeNodeIds() {
    sync::LockGuard guard(mutex_);
    std::vector<uint32_t> nodeIds;
    for (const auto& [nodeId, record] : records_) {
        if (record.active) {
            nodeIds.push_back(nodeId);
        }
    }
    return nodeIds;
}

std::vector<uint32_t> NodeTable::nodesWithFreeMemory(uint64_t requiredBytes, uint32_t requesterNodeId) {
    std::vector<NodeRecord> candidates;
    {
        sync::LockGuard guard(mutex_);
        for (const auto& [nodeId, record] : records_) {
            if (record.active && nodeId != requesterNodeId && record.freeMemoryBytes >= requiredBytes) {
                candidates.push_back(record);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const NodeRecord& left, const NodeRecord& right) {
        return left.freeMemoryBytes > right.freeMemoryBytes;
    });
    std::vector<uint32_t> nodeIds;
    for (const NodeRecord& candidate : candidates) {
        nodeIds.push_back(candidate.nodeId);
    }
    return nodeIds;
}

std::vector<uint32_t> NodeTable::takeExpired(std::chrono::milliseconds timeout, Clock::time_point now,
                                             uint32_t selfNodeId) {
    sync::LockGuard guard(mutex_);
    std::vector<uint32_t> expired;
    for (auto& [nodeId, record] : records_) {
        if (nodeId != selfNodeId && record.active && now - record.lastSeen >= timeout) {
            record.active = false;
            expired.push_back(nodeId);
        }
    }
    return expired;
}

std::vector<NodeRecord> NodeTable::snapshot() {
    sync::LockGuard guard(mutex_);
    std::vector<NodeRecord> records;
    for (const auto& entry : records_) {
        records.push_back(entry.second);
    }
    return records;
}

std::vector<NodeInfo> NodeTable::toNodeInfo() {
    std::vector<NodeInfo> nodes;
    for (const NodeRecord& record : snapshot()) {
        NodeInfo info{};
        info.node_id = record.nodeId;
        std::strncpy(info.ip, record.ip.c_str(), sizeof(info.ip) - 1);
        info.main_port = record.mainPort;
        info.heartbeat_port = record.heartbeatPort;
        info.shared_memory_bytes = record.sharedMemoryBytes;
        info.free_memory_bytes = record.freeMemoryBytes;
        info.range_start = record.rangeStart;
        info.is_active = record.active ? 1 : 0;
        nodes.push_back(info);
    }
    return nodes;
}

}  // namespace dsm::peers
