#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsm/dsm.h"

namespace {

constexpr size_t kMaxNodes = 256;
constexpr size_t kMaxPages = 1u << 20;

struct Options {
    std::string configPath = "config/cluster.json";
    int nodeId = -1;
    std::string command;
    long commandArgument = -1;
    bool allNodes = false;
};

void printUsage() {
    std::printf(
        "Usage: dsm_cli [--config <path>] [--node <id>] <command>\n"
        "Commands:\n"
        "  status                 Show the node table of the cluster\n"
        "  stats [<node-id>|all]  Show the statistics of the local node, another node or every node\n"
        "  pages <node-id>|all    Show the page table of a node or of every node\n");
}

bool parseNumber(const char* text, long& outValue) {
    char* end = nullptr;
    outValue = std::strtol(text, &end, 10);
    return end != text && *end == '\0' && outValue >= 0;
}

bool parseOptions(int argumentCount, char** arguments, Options& options) {
    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        long value = 0;
        if (argument == "--config" && index + 1 < argumentCount) {
            options.configPath = arguments[++index];
        } else if (argument == "--node" && index + 1 < argumentCount && parseNumber(arguments[++index], value)) {
            options.nodeId = static_cast<int>(value);
        } else if (options.command.empty()) {
            options.command = argument;
        } else if (options.commandArgument < 0 && !options.allNodes && argument == "all") {
            options.allNodes = true;
        } else if (options.commandArgument < 0 && !options.allNodes && parseNumber(arguments[index], value)) {
            options.commandArgument = value;
        } else {
            return false;
        }
    }
    const bool hasArgument = options.allNodes || options.commandArgument >= 0;
    if (options.command == "pages") {
        return hasArgument;
    }
    return options.command == "status" || options.command == "stats";
}

const char* stateName(uint8_t state) {
    switch (state) {
        case MSI_MODIFIED: return "M";
        case MSI_SHARED: return "S";
        default: return "I";
    }
}

int showStatus() {
    std::vector<NodeInfo> nodes(kMaxNodes);
    const int count = dsm_get_node_table(nodes.data(), nodes.size());
    if (count < 0) {
        std::printf("The node table is not available (status %d)\n", count);
        return 1;
    }
    std::printf("%-6s %-16s %-6s %-10s %-8s %-12s %-12s\n", "NODE", "IP", "MAIN", "HEARTBEAT", "STATE", "MEMORY",
                "FREE");
    for (int index = 0; index < count && static_cast<size_t>(index) < nodes.size(); ++index) {
        const NodeInfo& node = nodes[index];
        std::printf("%-6u %-16s %-6u %-10u %-8s %-12" PRIu64 " %-12" PRIu64 "\n", node.node_id, node.ip,
                    node.main_port, node.heartbeat_port, node.is_active ? "online" : "offline",
                    node.shared_memory_bytes, node.free_memory_bytes);
    }
    return 0;
}

std::vector<NodeInfo> activeNodes() {
    std::vector<NodeInfo> nodes(kMaxNodes);
    const int count = dsm_get_node_table(nodes.data(), nodes.size());
    std::vector<NodeInfo> active;
    for (int index = 0; index < count && static_cast<size_t>(index) < nodes.size(); ++index) {
        if (nodes[index].is_active) {
            active.push_back(nodes[index]);
        }
    }
    return active;
}

int showStats(int nodeId) {
    DsmStats stats{};
    const int status = nodeId < 0 ? dsm_get_stats(&stats) : dsm_get_node_stats(nodeId, &stats);
    if (status != DSM_OK) {
        std::printf("Statistics are not available (status %d)\n", status);
        return 1;
    }
    if (nodeId >= 0) {
        std::printf("Node %d statistics\n", nodeId);
    }
    std::printf("Page faults        %" PRIu64 "\n", stats.page_faults);
    std::printf("Pages sent         %" PRIu64 "\n", stats.pages_sent);
    std::printf("Pages received     %" PRIu64 "\n", stats.pages_received);
    std::printf("Invalidations      %" PRIu64 "\n", stats.invalidations);
    std::printf("Messages sent      %" PRIu64 "\n", stats.messages_sent);
    std::printf("Messages received  %" PRIu64 "\n", stats.messages_received);
    std::printf("Used memory        %" PRIu64 " bytes\n", stats.used_memory_bytes);
    std::printf("Free memory        %" PRIu64 " bytes\n", stats.free_memory_bytes);
    return 0;
}

void addStats(DsmStats& total, const DsmStats& stats) {
    total.page_faults += stats.page_faults;
    total.pages_sent += stats.pages_sent;
    total.pages_received += stats.pages_received;
    total.invalidations += stats.invalidations;
    total.messages_sent += stats.messages_sent;
    total.messages_received += stats.messages_received;
    total.used_memory_bytes += stats.used_memory_bytes;
    total.free_memory_bytes += stats.free_memory_bytes;
}

void printStatsRow(const char* label, const DsmStats& stats) {
    std::printf("%-6s %-8" PRIu64 " %-8" PRIu64 " %-8" PRIu64 " %-8" PRIu64 " %-10" PRIu64 " %-10" PRIu64
                " %-12" PRIu64 " %-12" PRIu64 "\n",
                label, stats.page_faults, stats.pages_sent, stats.pages_received, stats.invalidations,
                stats.messages_sent, stats.messages_received, stats.used_memory_bytes, stats.free_memory_bytes);
}

int showAllStats() {
    std::printf("Statistics of every active node\n");
    std::printf("%-6s %-8s %-8s %-8s %-8s %-10s %-10s %-12s %-12s\n", "NODE", "FAULTS", "SENT", "RECEIVED",
                "INVALID", "MSG_SENT", "MSG_RECV", "USED", "FREE");
    DsmStats total{};
    int result = 0;
    for (const NodeInfo& node : activeNodes()) {
        DsmStats stats{};
        const int status = dsm_get_node_stats(static_cast<int>(node.node_id), &stats);
        const std::string label = std::to_string(node.node_id);
        if (status != DSM_OK) {
            std::printf("%-6s not available (status %d)\n", label.c_str(), status);
            result = 1;
            continue;
        }
        printStatsRow(label.c_str(), stats);
        addStats(total, stats);
    }
    printStatsRow("TOTAL", total);
    return result;
}

int showPages(int nodeId) {
    std::vector<PageTableEntry> entries(kMaxPages);
    const int count = dsm_get_page_table(nodeId, entries.data(), entries.size());
    if (count < 0) {
        std::printf("The page table of node %d is not available (status %d)\n", nodeId, count);
        return 1;
    }
    std::printf("Node %d page table (%d pages)\n", nodeId, count);
    std::printf("%-10s %-6s %-6s %-8s %-6s %-8s %-6s %-6s\n", "PAGE", "HOME", "STORE", "HOLDER", "STATE", "PROCESS",
                "ORDER", "COPIES");
    for (int index = 0; index < count && static_cast<size_t>(index) < entries.size(); ++index) {
        const PageTableEntry& entry = entries[index];
        std::printf("%-10" PRIu64 " %-6u %-6u %-8u %-6s %-8u %-6u %-6u\n", entry.global_page_id, entry.home_node,
                    entry.storage_node, entry.current_holder, stateName(entry.state), entry.process_id,
                    entry.page_order, entry.copy_count);
    }
    return 0;
}

int showAllPages() {
    int result = 0;
    for (const NodeInfo& node : activeNodes()) {
        result |= showPages(static_cast<int>(node.node_id));
        std::printf("\n");
    }
    return result;
}

}  // namespace

int main(int argumentCount, char** arguments) {
    Options options;
    if (!parseOptions(argumentCount, arguments, options)) {
        printUsage();
        return 2;
    }
    const int status = dsm_init(options.configPath.c_str(), options.nodeId, 0);
    if (status == DSM_ERR_NODE_UNAVAILABLE) {
        std::printf("No DSM node is running on this machine. Start one with dsm_node.\n");
        return 1;
    }
    if (status == DSM_ERR_INVALID_ARGUMENT) {
        std::printf("The requested node is not in the configuration or does not run on this machine.\n"
                    "Applications can only attach to a node of their own machine.\n");
        return 1;
    }
    if (status != DSM_OK) {
        std::printf("dsm_init failed with status %d\n", status);
        return 1;
    }
    int result = 0;
    if (options.command == "status") {
        result = showStatus();
    } else if (options.command == "stats") {
        result = options.allNodes ? showAllStats() : showStats(static_cast<int>(options.commandArgument));
    } else {
        result = options.allNodes ? showAllPages() : showPages(static_cast<int>(options.commandArgument));
    }
    dsm_finalize();
    return result;
}
