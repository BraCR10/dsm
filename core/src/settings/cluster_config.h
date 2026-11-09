#ifndef DSM_SETTINGS_CLUSTER_CONFIG_H
#define DSM_SETTINGS_CLUSTER_CONFIG_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dsm/dsm.h"

namespace dsm::settings {

constexpr size_t kConfigHashSize = 32;
using ConfigHash = std::array<uint8_t, kConfigHashSize>;

struct NodeConfig {
    uint32_t nodeId = 0;
    std::string ip;
    uint16_t mainPort = 0;
    uint16_t heartbeatPort = 0;
    uint64_t memorySize = 0;
};

struct ClusterConfig {
    uint64_t baseAddress = 0;
    uint64_t pageSize = 0;
    uint64_t addressWindowSize = 0;
    uint64_t nodeRangeSpacing = 0;
    uint64_t initialMemorySize = 0;
    uint32_t heartbeatIntervalMs = 0;
    uint32_t heartbeatTimeoutSec = 0;
    std::vector<NodeConfig> nodes;
    ConfigHash configHash{};

    const NodeConfig* findNode(uint32_t nodeId) const;
};

struct ConfigResult {
    DsmStatus status = DSM_OK;
    std::string errorMessage;
};

ConfigResult parseClusterConfig(const std::string& jsonText, ClusterConfig& outConfig);
ConfigResult loadClusterConfig(const std::string& path, ClusterConfig& outConfig);

}  // namespace dsm::settings

#endif
