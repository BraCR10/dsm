#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "api/node_runtime.h"
#include "common/log.h"
#include "settings/cluster_config.h"

namespace {

constexpr const char* kModule = "node";

struct Options {
    std::string configPath = "config/cluster.json";
    std::optional<uint32_t> nodeId;
    uint64_t memoryBytes = 0;
};

void printUsage() {
    std::printf(
        "Usage: dsm_node [--config <path>] [--node <id>] [--memory <bytes>]\n"
        "  Without --node the node takes the first configuration entry whose IP belongs to this machine\n"
        "  and whose ports are free. Without --memory it uses the memory_size of its entry.\n");
}

bool parseUnsigned(const char* text, uint64_t& outValue) {
    char* end = nullptr;
    outValue = std::strtoull(text, &end, 10);
    return end != text && *end == '\0';
}

bool parseOptions(int argumentCount, char** arguments, Options& options) {
    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        const bool hasValue = index + 1 < argumentCount;
        uint64_t value = 0;
        if (argument == "--config" && hasValue) {
            options.configPath = arguments[++index];
        } else if (argument == "--node" && hasValue && parseUnsigned(arguments[++index], value)) {
            options.nodeId = static_cast<uint32_t>(value);
        } else if (argument == "--memory" && hasValue && parseUnsigned(arguments[++index], value)) {
            options.memoryBytes = value;
        } else {
            return false;
        }
    }
    return true;
}

void waitForTerminationSignal() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int received = 0;
    sigwait(&signals, &received);
}

}  // namespace

int main(int argumentCount, char** arguments) {
    Options options;
    if (!parseOptions(argumentCount, arguments, options)) {
        printUsage();
        return 2;
    }
    dsm::settings::ClusterConfig config;
    const dsm::settings::ConfigResult result = dsm::settings::loadClusterConfig(options.configPath, config);
    if (result.status != DSM_OK) {
        dsm::common::logError(kModule, dsm::common::fromSelf(), "%s", result.errorMessage.c_str());
        return 1;
    }
    const std::optional<uint32_t> nodeId = options.nodeId ? options.nodeId : dsm::api::detectLocalNodeId(config);
    const dsm::settings::NodeConfig* self = nodeId ? config.findNode(*nodeId) : nullptr;
    if (self == nullptr) {
        dsm::common::logError(kModule, dsm::common::fromSelf(), "no configuration entry is available for this machine");
        return 1;
    }
    dsm::common::setLogNodeId(*nodeId);
    const uint64_t memoryBytes = options.memoryBytes != 0 ? options.memoryBytes : self->memorySize;
    if (memoryBytes % config.pageSize != 0 || memoryBytes > config.nodeRangeSpacing) {
        dsm::common::logError(kModule, dsm::common::fromSelf(),
                              "--memory must be a multiple of page_size and fit in node_range_spacing");
        return 1;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    dsm::api::NodeRuntime node(std::move(config), *nodeId, memoryBytes);
    const DsmStatus status = node.start();
    if (status != DSM_OK) {
        dsm::common::logError(kModule, dsm::common::fromSelf(), "node could not start (status %d)", status);
        return 1;
    }
    dsm::common::logInfo(kModule, dsm::common::fromSelf(),
                         "running with %llu bytes of shared memory, press Ctrl+C to stop",
                         static_cast<unsigned long long>(memoryBytes));
    waitForTerminationSignal();
    node.shutdown();
    return 0;
}
