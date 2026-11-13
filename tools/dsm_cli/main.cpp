#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "msi/msi_policy.h"
#include "network/connection.h"
#include "network/message.h"
#include "settings/cluster_config.h"
#include "stats/stats_messages.h"

namespace {

using namespace dsm;

constexpr uint32_t kConnectTimeoutMs = 1000;

void printUsage() {
    std::printf(
        "Usage: dsm_cli --config <path> --node <id> <command>\n"
        "Commands:\n"
        "  stats   Show the statistics of the node\n"
        "  pages   Show the page table of the node\n"
        "  status  Show the statistics of every node in the configuration\n");
}

bool queryNode(const settings::NodeConfig& node, network::MessageType requestType, network::Message& outResponse) {
    auto connection = network::Connection::connectTo(node.ip, node.mainPort, network::Channel::Main, kConnectTimeoutMs);
    if (!connection) {
        return false;
    }
    network::Message request = network::makeMessage(requestType);
    request.header.sourceNode = network::kExternalClientNodeId;
    request.header.destinationNode = node.nodeId;
    request.header.requestId = 1;
    return connection->send(request) && connection->receive(outResponse);
}

void printStats(uint32_t nodeId, const DsmStats& stats) {
    std::printf("Node %u statistics\n", nodeId);
    std::printf("  Page faults        %llu\n", static_cast<unsigned long long>(stats.page_faults));
    std::printf("  Pages sent         %llu\n", static_cast<unsigned long long>(stats.pages_sent));
    std::printf("  Pages received     %llu\n", static_cast<unsigned long long>(stats.pages_received));
    std::printf("  Invalidations      %llu\n", static_cast<unsigned long long>(stats.invalidations));
    std::printf("  Messages sent      %llu\n", static_cast<unsigned long long>(stats.messages_sent));
    std::printf("  Messages received  %llu\n", static_cast<unsigned long long>(stats.messages_received));
    std::printf("  Used memory        %llu bytes\n", static_cast<unsigned long long>(stats.used_memory_bytes));
    std::printf("  Free memory        %llu bytes\n", static_cast<unsigned long long>(stats.free_memory_bytes));
}

int showStats(const settings::NodeConfig& node) {
    network::Message response;
    if (!queryNode(node, network::MessageType::StatsRequest, response)) {
        std::printf("Node %u is not reachable at %s:%u\n", node.nodeId, node.ip.c_str(), node.mainPort);
        return 1;
    }
    const auto stats = stats::decodeStats(response.payload);
    if (!stats) {
        std::printf("Node %u sent an invalid STATS_DATA message\n", node.nodeId);
        return 1;
    }
    printStats(node.nodeId, *stats);
    return 0;
}

int showPages(const settings::NodeConfig& node) {
    network::Message response;
    if (!queryNode(node, network::MessageType::PageTableRequest, response)) {
        std::printf("Node %u is not reachable at %s:%u\n", node.nodeId, node.ip.c_str(), node.mainPort);
        return 1;
    }
    const auto entries = stats::decodePageTable(response.payload);
    if (!entries) {
        std::printf("Node %u sent an invalid PAGE_TABLE_DATA message\n", node.nodeId);
        return 1;
    }
    std::printf("Node %u page table (%zu pages)\n", node.nodeId, entries->size());
    std::printf("  %-10s %-6s %-6s %-8s %-6s %-8s %-6s %-6s\n", "PAGE", "HOME", "STORE", "HOLDER", "STATE", "PROCESS",
                "ORDER", "COPIES");
    for (const PageTableEntry& entry : *entries) {
        std::printf("  %-10llu %-6u %-6u %-8u %-6s %-8u %-6u %-6u\n",
                    static_cast<unsigned long long>(entry.global_page_id), entry.home_node, entry.storage_node,
                    entry.current_holder, msi::msiStateName(static_cast<MsiState>(entry.state)), entry.process_id,
                    entry.page_order, entry.copy_count);
    }
    return 0;
}

int showClusterStatus(const settings::ClusterConfig& config) {
    for (const settings::NodeConfig& node : config.nodes) {
        network::Message response;
        if (!queryNode(node, network::MessageType::StatsRequest, response)) {
            std::printf("Node %u  offline\n", node.nodeId);
            continue;
        }
        const auto stats = stats::decodeStats(response.payload);
        if (stats) {
            std::printf("Node %u  online  used %llu bytes  free %llu bytes  faults %llu\n", node.nodeId,
                        static_cast<unsigned long long>(stats->used_memory_bytes),
                        static_cast<unsigned long long>(stats->free_memory_bytes),
                        static_cast<unsigned long long>(stats->page_faults));
        }
    }
    return 0;
}

}  // namespace

int main(int argumentCount, char** arguments) {
    std::string configPath;
    int nodeId = -1;
    std::string command;
    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        if (argument == "--config" && index + 1 < argumentCount) {
            configPath = arguments[++index];
        } else if (argument == "--node" && index + 1 < argumentCount) {
            nodeId = std::atoi(arguments[++index]);
        } else {
            command = argument;
        }
    }
    if (configPath.empty() || command.empty()) {
        printUsage();
        return 2;
    }
    settings::ClusterConfig config;
    const settings::ConfigResult result = settings::loadClusterConfig(configPath, config);
    if (result.status != DSM_OK) {
        std::printf("%s\n", result.errorMessage.c_str());
        return 1;
    }
    if (command == "status") {
        return showClusterStatus(config);
    }
    const settings::NodeConfig* node = nodeId < 0 ? nullptr : config.findNode(static_cast<uint32_t>(nodeId));
    if (node == nullptr) {
        std::printf("Node %d is not in the configuration\n", nodeId);
        return 1;
    }
    if (command == "stats") {
        return showStats(*node);
    }
    if (command == "pages") {
        return showPages(*node);
    }
    printUsage();
    return 2;
}
