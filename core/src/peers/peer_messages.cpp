#include "peers/peer_messages.h"

namespace dsm::peers {

namespace {

using network::ByteReader;
using network::ByteWriter;

void writeNodeRecord(ByteWriter& writer, const NodeRecord& node) {
    writer.writeU32(node.nodeId);
    writer.writeFixedString(node.ip, kIpFieldSize);
    writer.writeU16(node.mainPort);
    writer.writeU16(node.heartbeatPort);
    writer.writeU64(node.sharedMemoryBytes);
    writer.writeU64(node.freeMemoryBytes);
    writer.writeU64(node.rangeStart);
}

NodeRecord readNodeRecord(ByteReader& reader) {
    NodeRecord node;
    node.nodeId = reader.readU32();
    node.ip = reader.readFixedString(kIpFieldSize);
    node.mainPort = reader.readU16();
    node.heartbeatPort = reader.readU16();
    node.sharedMemoryBytes = reader.readU64();
    node.freeMemoryBytes = reader.readU64();
    node.rangeStart = reader.readU64();
    return node;
}

template <typename Value>
std::optional<Value> completeRead(const ByteReader& reader, Value value) {
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::vector<uint8_t> encodeNodeJoin(const NodeJoinPayload& payload) {
    ByteWriter writer;
    writeNodeRecord(writer, payload.node);
    writer.writeBytes(payload.configHash.data(), payload.configHash.size());
    return writer.take();
}

std::optional<NodeJoinPayload> decodeNodeJoin(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    NodeJoinPayload payload;
    payload.node = readNodeRecord(reader);
    reader.readBytes(payload.configHash.data(), payload.configHash.size());
    return completeRead(reader, payload);
}

std::vector<uint8_t> encodeNodeList(const std::vector<NodeRecord>& nodes) {
    ByteWriter writer;
    writer.writeU32(static_cast<uint32_t>(nodes.size()));
    for (const NodeRecord& node : nodes) {
        writeNodeRecord(writer, node);
    }
    return writer.take();
}

std::optional<std::vector<NodeRecord>> decodeNodeList(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint32_t count = reader.readU32();
    std::vector<NodeRecord> nodes;
    for (uint32_t index = 0; index < count && reader.ok(); ++index) {
        nodes.push_back(readNodeRecord(reader));
    }
    return completeRead(reader, nodes);
}

std::vector<uint8_t> encodeHeartbeat(const HeartbeatPayload& payload) {
    ByteWriter writer;
    writer.writeU32(payload.nodeId);
    writer.writeU64(payload.freeMemoryBytes);
    return writer.take();
}

std::optional<HeartbeatPayload> decodeHeartbeat(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    HeartbeatPayload payload;
    payload.nodeId = reader.readU32();
    payload.freeMemoryBytes = reader.readU64();
    return completeRead(reader, payload);
}

std::vector<uint8_t> encodeNodeId(uint32_t nodeId) {
    ByteWriter writer;
    writer.writeU32(nodeId);
    return writer.take();
}

std::optional<uint32_t> decodeNodeId(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint32_t nodeId = reader.readU32();
    return completeRead(reader, nodeId);
}

}  // namespace dsm::peers
