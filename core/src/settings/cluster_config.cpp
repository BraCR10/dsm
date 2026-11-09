#include "settings/cluster_config.h"

#include <openssl/evp.h>

#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "nlohmann/json.hpp"

namespace dsm::settings {

namespace {

using Json = nlohmann::json;

constexpr uint64_t kMinimumPageSize = 4096;

ConfigResult configError(std::string message) {
    return {DSM_ERR_CONFIG, std::move(message)};
}

bool isPowerOfTwo(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

uint64_t parseAddress(const Json& value) {
    if (value.is_string()) {
        return std::stoull(value.get<std::string>(), nullptr, 0);
    }
    return value.get<uint64_t>();
}

ConfigHash computeConfigHash(const Json& document) {
    const std::string canonicalText = document.dump();
    ConfigHash hash{};
    unsigned int hashLength = 0;
    EVP_Digest(canonicalText.data(), canonicalText.size(), hash.data(), &hashLength, EVP_sha256(), nullptr);
    return hash;
}

void readClusterSection(const Json& cluster, ClusterConfig& config) {
    config.baseAddress = parseAddress(cluster.at("base_address"));
    config.pageSize = cluster.at("page_size").get<uint64_t>();
    config.addressWindowSize = cluster.at("address_window_size").get<uint64_t>();
    config.nodeRangeSpacing = cluster.at("node_range_spacing").get<uint64_t>();
    config.initialMemorySize = cluster.at("initial_memory_size").get<uint64_t>();
}

void readHeartbeatSection(const Json& heartbeat, ClusterConfig& config) {
    config.heartbeatIntervalMs = heartbeat.at("interval_ms").get<uint32_t>();
    config.heartbeatTimeoutSec = heartbeat.at("timeout_sec").get<uint32_t>();
}

NodeConfig readNode(const Json& node, uint64_t defaultMemorySize) {
    NodeConfig nodeConfig;
    nodeConfig.nodeId = node.at("node_id").get<uint32_t>();
    nodeConfig.ip = node.at("ip").get<std::string>();
    nodeConfig.mainPort = node.at("ports").at("main").get<uint16_t>();
    nodeConfig.heartbeatPort = node.at("ports").at("heartbeat").get<uint16_t>();
    nodeConfig.memorySize = node.value("memory_size", defaultMemorySize);
    return nodeConfig;
}

ConfigResult validateClusterLayout(const ClusterConfig& config) {
    if (!isPowerOfTwo(config.pageSize) || config.pageSize < kMinimumPageSize) {
        return configError("page_size must be a power of two and at least 4096");
    }
    if (config.baseAddress % config.pageSize != 0) {
        return configError("base_address must be aligned to page_size");
    }
    if (config.nodeRangeSpacing == 0 || config.nodeRangeSpacing % config.pageSize != 0) {
        return configError("node_range_spacing must be a non-zero multiple of page_size");
    }
    if (config.heartbeatIntervalMs == 0 || config.heartbeatTimeoutSec == 0) {
        return configError("heartbeat interval_ms and timeout_sec must be greater than zero");
    }
    if (config.nodes.empty()) {
        return configError("nodes must contain at least one node");
    }
    return {};
}

ConfigResult validateNodes(const ClusterConfig& config) {
    std::set<uint32_t> nodeIds;
    std::set<std::pair<std::string, uint16_t>> endpoints;
    for (const NodeConfig& node : config.nodes) {
        if (!nodeIds.insert(node.nodeId).second) {
            return configError("duplicate node_id " + std::to_string(node.nodeId));
        }
        if (!endpoints.insert({node.ip, node.mainPort}).second ||
            !endpoints.insert({node.ip, node.heartbeatPort}).second) {
            return configError("node " + std::to_string(node.nodeId) + " reuses a port of another node");
        }
        if (node.memorySize == 0 || node.memorySize % config.pageSize != 0) {
            return configError("memory_size of node " + std::to_string(node.nodeId) +
                               " must be a non-zero multiple of page_size");
        }
        if (node.memorySize > config.nodeRangeSpacing) {
            return configError("memory_size of node " + std::to_string(node.nodeId) +
                               " exceeds node_range_spacing");
        }
        const uint64_t regionEnd = (static_cast<uint64_t>(node.nodeId) + 1) * config.nodeRangeSpacing;
        if (regionEnd > config.addressWindowSize) {
            return configError("region of node " + std::to_string(node.nodeId) +
                               " does not fit inside address_window_size");
        }
    }
    return {};
}

}  // namespace

const NodeConfig* ClusterConfig::findNode(uint32_t nodeId) const {
    for (const NodeConfig& node : nodes) {
        if (node.nodeId == nodeId) {
            return &node;
        }
    }
    return nullptr;
}

ConfigResult parseClusterConfig(const std::string& jsonText, ClusterConfig& outConfig) {
    ClusterConfig config;
    try {
        const Json document = Json::parse(jsonText);
        readClusterSection(document.at("cluster"), config);
        readHeartbeatSection(document.at("heartbeat"), config);
        for (const Json& node : document.at("nodes")) {
            config.nodes.push_back(readNode(node, config.initialMemorySize));
        }
        config.configHash = computeConfigHash(document);
    } catch (const std::exception& error) {
        return configError(std::string("invalid configuration: ") + error.what());
    }

    ConfigResult layoutResult = validateClusterLayout(config);
    if (layoutResult.status != DSM_OK) {
        return layoutResult;
    }
    ConfigResult nodesResult = validateNodes(config);
    if (nodesResult.status != DSM_OK) {
        return nodesResult;
    }
    outConfig = std::move(config);
    return {};
}

ConfigResult loadClusterConfig(const std::string& path, ClusterConfig& outConfig) {
    std::ifstream file(path);
    if (!file) {
        return configError("cannot open configuration file " + path);
    }
    std::stringstream content;
    content << file.rdbuf();
    return parseClusterConfig(content.str(), outConfig);
}

}  // namespace dsm::settings
