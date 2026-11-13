#include "stats/stats_module.h"

#include "stats/stats_messages.h"

namespace dsm::stats {

namespace {

constexpr uint32_t kQueryTimeoutMs = 2000;

using network::Channel;
using network::IncomingMessage;
using network::makeMessage;
using network::Message;
using network::MessageType;

}  // namespace

StatsModule::StatsModule(network::NetworkModule& network, memory::MemoryModule& memory)
    : network_(network), memory_(memory) {}

void StatsModule::registerHandlers() {
    network_.registerHandler(MessageType::StatsRequest, [this](const IncomingMessage& incoming) {
        network_.reply(incoming, makeMessage(MessageType::StatsData, encodeStats(localStats())));
    });
    network_.registerHandler(MessageType::PageTableRequest, [this](const IncomingMessage& incoming) {
        network_.reply(incoming, makeMessage(MessageType::PageTableData, encodePageTable(memory_.pageTableView())));
    });
}

DsmStats StatsModule::localStats() {
    const memory::MemoryCounters& counters = memory_.counters();
    DsmStats stats{};
    stats.page_faults = counters.pageFaults.load();
    stats.pages_sent = counters.pagesSent.load();
    stats.pages_received = counters.pagesReceived.load();
    stats.invalidations = counters.invalidations.load();
    stats.messages_sent = network_.messagesSent();
    stats.messages_received = network_.messagesReceived();
    stats.used_memory_bytes = memory_.usedBytes();
    stats.free_memory_bytes = memory_.freeBytes();
    return stats;
}

DsmStatus StatsModule::remoteStats(uint32_t nodeId, DsmStats& outStats) {
    Message response;
    const DsmStatus status =
        network_.request(nodeId, Channel::Main, makeMessage(MessageType::StatsRequest), kQueryTimeoutMs, response);
    if (status != DSM_OK) {
        return status;
    }
    const auto stats = decodeStats(response.payload);
    if (!stats) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outStats = *stats;
    return DSM_OK;
}

DsmStatus StatsModule::remotePageTable(uint32_t nodeId, std::vector<PageTableEntry>& outEntries) {
    Message response;
    const DsmStatus status = network_.request(nodeId, Channel::Main, makeMessage(MessageType::PageTableRequest),
                                              kQueryTimeoutMs, response);
    if (status != DSM_OK) {
        return status;
    }
    const auto entries = decodePageTable(response.payload);
    if (!entries) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outEntries = *entries;
    return DSM_OK;
}

}  // namespace dsm::stats
