#include "stats/stats_messages.h"

#include "network/message.h"

namespace dsm::stats {

using network::ByteReader;
using network::ByteWriter;

std::vector<uint8_t> encodeStats(const DsmStats& stats) {
    ByteWriter writer;
    writer.writeU64(stats.page_faults);
    writer.writeU64(stats.pages_sent);
    writer.writeU64(stats.pages_received);
    writer.writeU64(stats.invalidations);
    writer.writeU64(stats.messages_sent);
    writer.writeU64(stats.messages_received);
    writer.writeU64(stats.used_memory_bytes);
    writer.writeU64(stats.free_memory_bytes);
    return writer.take();
}

std::optional<DsmStats> decodeStats(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    DsmStats stats{};
    stats.page_faults = reader.readU64();
    stats.pages_sent = reader.readU64();
    stats.pages_received = reader.readU64();
    stats.invalidations = reader.readU64();
    stats.messages_sent = reader.readU64();
    stats.messages_received = reader.readU64();
    stats.used_memory_bytes = reader.readU64();
    stats.free_memory_bytes = reader.readU64();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return stats;
}

std::vector<uint8_t> encodePageTable(const std::vector<PageTableEntry>& entries) {
    ByteWriter writer;
    writer.writeU32(static_cast<uint32_t>(entries.size()));
    for (const PageTableEntry& entry : entries) {
        writer.writeU64(entry.global_page_id);
        writer.writeU32(entry.home_node);
        writer.writeU32(entry.storage_node);
        writer.writeU32(entry.current_holder);
        writer.writeU32(entry.process_id);
        writer.writeU32(entry.page_order);
        writer.writeU32(entry.copy_count);
        writer.writeU8(entry.state);
    }
    return writer.take();
}

std::optional<std::vector<PageTableEntry>> decodePageTable(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint32_t count = reader.readU32();
    std::vector<PageTableEntry> entries;
    for (uint32_t index = 0; index < count && reader.ok(); ++index) {
        PageTableEntry entry{};
        entry.global_page_id = reader.readU64();
        entry.home_node = reader.readU32();
        entry.storage_node = reader.readU32();
        entry.current_holder = reader.readU32();
        entry.process_id = reader.readU32();
        entry.page_order = reader.readU32();
        entry.copy_count = reader.readU32();
        entry.state = reader.readU8();
        entries.push_back(entry);
    }
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return entries;
}

}  // namespace dsm::stats
