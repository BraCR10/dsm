#include "api/local_messages.h"

#include <cstring>

#include "network/message.h"

namespace dsm::api {

namespace {

using network::ByteReader;
using network::ByteWriter;

constexpr size_t kIpFieldSize = sizeof(NodeInfo::ip);

template <typename Value>
std::optional<Value> completeRead(const ByteReader& reader, Value value) {
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::vector<uint8_t> encodeAttach(const AttachPayload& payload) {
    ByteWriter writer;
    writer.writeU32(payload.processId);
    writer.writeBytes(payload.configHash.data(), payload.configHash.size());
    return writer.take();
}

std::optional<AttachPayload> decodeAttach(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    AttachPayload payload;
    payload.processId = reader.readU32();
    reader.readBytes(payload.configHash.data(), payload.configHash.size());
    return completeRead(reader, payload);
}

std::vector<uint8_t> encodeFault(const FaultPayload& payload) {
    ByteWriter writer;
    writer.writeU64(payload.address);
    writer.writeU8(payload.isWrite ? 1 : 0);
    return writer.take();
}

std::optional<FaultPayload> decodeFault(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    FaultPayload payload;
    payload.address = reader.readU64();
    payload.isWrite = reader.readU8() != 0;
    return completeRead(reader, payload);
}

std::vector<uint8_t> encodeProtection(const ProtectionPayload& payload) {
    ByteWriter writer;
    writer.writeU64(payload.pageId);
    writer.writeU32(payload.protection);
    return writer.take();
}

std::optional<ProtectionPayload> decodeProtection(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    ProtectionPayload payload;
    payload.pageId = reader.readU64();
    payload.protection = reader.readU32();
    return completeRead(reader, payload);
}

std::vector<uint8_t> encodeValue(uint64_t value) {
    ByteWriter writer;
    writer.writeU64(value);
    return writer.take();
}

std::optional<uint64_t> decodeValue(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint64_t value = reader.readU64();
    return completeRead(reader, value);
}

std::vector<uint8_t> encodeNodeTable(const std::vector<NodeInfo>& nodes) {
    ByteWriter writer;
    writer.writeU32(static_cast<uint32_t>(nodes.size()));
    for (const NodeInfo& node : nodes) {
        writer.writeU32(node.node_id);
        writer.writeFixedString(node.ip, kIpFieldSize);
        writer.writeU16(node.main_port);
        writer.writeU16(node.heartbeat_port);
        writer.writeU64(node.shared_memory_bytes);
        writer.writeU64(node.free_memory_bytes);
        writer.writeU64(node.range_start);
        writer.writeU8(node.is_active);
    }
    return writer.take();
}

std::optional<std::vector<NodeInfo>> decodeNodeTable(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint32_t count = reader.readU32();
    std::vector<NodeInfo> nodes;
    for (uint32_t index = 0; index < count && reader.ok(); ++index) {
        NodeInfo node{};
        node.node_id = reader.readU32();
        const std::string ip = reader.readFixedString(kIpFieldSize);
        std::strncpy(node.ip, ip.c_str(), kIpFieldSize - 1);
        node.main_port = reader.readU16();
        node.heartbeat_port = reader.readU16();
        node.shared_memory_bytes = reader.readU64();
        node.free_memory_bytes = reader.readU64();
        node.range_start = reader.readU64();
        node.is_active = reader.readU8();
        nodes.push_back(node);
    }
    return completeRead(reader, nodes);
}

}  // namespace dsm::api
