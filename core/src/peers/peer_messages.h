#ifndef DSM_PEERS_PEER_MESSAGES_H
#define DSM_PEERS_PEER_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

#include "network/message.h"
#include "peers/node_table.h"
#include "settings/cluster_config.h"

namespace dsm::peers {

constexpr size_t kIpFieldSize = 46;

struct NodeJoinPayload {
    NodeRecord node;
    settings::ConfigHash configHash{};
};

struct HeartbeatPayload {
    uint32_t nodeId = 0;
    uint64_t freeMemoryBytes = 0;
};

std::vector<uint8_t> encodeNodeJoin(const NodeJoinPayload& payload);
std::optional<NodeJoinPayload> decodeNodeJoin(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeNodeList(const std::vector<NodeRecord>& nodes);
std::optional<std::vector<NodeRecord>> decodeNodeList(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeHeartbeat(const HeartbeatPayload& payload);
std::optional<HeartbeatPayload> decodeHeartbeat(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeNodeId(uint32_t nodeId);
std::optional<uint32_t> decodeNodeId(const std::vector<uint8_t>& bytes);

}  // namespace dsm::peers

#endif
