#include "network/message.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

namespace dsm::network {

namespace {

uint64_t hostToNetwork64(uint64_t value) {
    const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
    const uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFFu));
    return (static_cast<uint64_t>(low) << 32) | high;
}

uint64_t networkToHost64(uint64_t value) {
    return hostToNetwork64(value);
}

template <typename Value>
void storeAt(uint8_t* buffer, size_t offset, Value value) {
    std::memcpy(buffer + offset, &value, sizeof(Value));
}

template <typename Value>
Value loadAt(const uint8_t* buffer, size_t offset) {
    Value value;
    std::memcpy(&value, buffer + offset, sizeof(Value));
    return value;
}

}  // namespace

Message makeMessage(MessageType type, std::vector<uint8_t> payload) {
    Message message;
    message.header.type = static_cast<uint16_t>(type);
    message.header.payloadSize = static_cast<uint32_t>(payload.size());
    message.payload = std::move(payload);
    return message;
}

bool isResponseType(MessageType type) {
    switch (type) {
        case MessageType::NodeJoinAck:
        case MessageType::MemoryGrant:
        case MessageType::PageData:
        case MessageType::InvalidateAck:
        case MessageType::NameAck:
        case MessageType::LockGrant:
        case MessageType::StatsData:
        case MessageType::PageTableData:
            return true;
        default:
            return false;
    }
}

const char* messageTypeName(MessageType type) {
    switch (type) {
        case MessageType::NodeJoin: return "NODE_JOIN";
        case MessageType::NodeJoinAck: return "NODE_JOIN_ACK";
        case MessageType::NodeLeave: return "NODE_LEAVE";
        case MessageType::NodeDown: return "NODE_DOWN";
        case MessageType::Heartbeat: return "HEARTBEAT";
        case MessageType::HeartbeatProbe: return "HEARTBEAT_PROBE";
        case MessageType::MemoryRequest: return "MEMORY_REQUEST";
        case MessageType::MemoryGrant: return "MEMORY_GRANT";
        case MessageType::PageRequest: return "PAGE_REQUEST";
        case MessageType::PageData: return "PAGE_DATA";
        case MessageType::PageRecall: return "PAGE_RECALL";
        case MessageType::Invalidate: return "INVALIDATE";
        case MessageType::InvalidateAck: return "INVALIDATE_ACK";
        case MessageType::NamePublish: return "NAME_PUBLISH";
        case MessageType::NameAck: return "NAME_ACK";
        case MessageType::NameUnlink: return "NAME_UNLINK";
        case MessageType::LockRequest: return "LOCK_REQUEST";
        case MessageType::LockGrant: return "LOCK_GRANT";
        case MessageType::LockRelease: return "LOCK_RELEASE";
        case MessageType::BarrierArrive: return "BARRIER_ARRIVE";
        case MessageType::BarrierRelease: return "BARRIER_RELEASE";
        case MessageType::StatsRequest: return "STATS_REQUEST";
        case MessageType::StatsData: return "STATS_DATA";
        case MessageType::PageTableRequest: return "PAGE_TABLE_REQUEST";
        case MessageType::PageTableData: return "PAGE_TABLE_DATA";
    }
    return "UNKNOWN";
}

void encodeHeader(const MessageHeader& header, uint8_t (&buffer)[kHeaderSize]) {
    storeAt(buffer, 0, htonl(header.magic));
    storeAt(buffer, 4, htons(header.version));
    storeAt(buffer, 6, htons(header.type));
    storeAt(buffer, 8, htonl(header.sourceNode));
    storeAt(buffer, 12, htonl(header.destinationNode));
    storeAt(buffer, 16, hostToNetwork64(header.requestId));
    storeAt(buffer, 24, htonl(header.payloadSize));
    storeAt(buffer, 28, htonl(static_cast<uint32_t>(header.status)));
}

bool decodeHeader(const uint8_t (&buffer)[kHeaderSize], MessageHeader& outHeader) {
    MessageHeader header;
    header.magic = ntohl(loadAt<uint32_t>(buffer, 0));
    header.version = ntohs(loadAt<uint16_t>(buffer, 4));
    header.type = ntohs(loadAt<uint16_t>(buffer, 6));
    header.sourceNode = ntohl(loadAt<uint32_t>(buffer, 8));
    header.destinationNode = ntohl(loadAt<uint32_t>(buffer, 12));
    header.requestId = networkToHost64(loadAt<uint64_t>(buffer, 16));
    header.payloadSize = ntohl(loadAt<uint32_t>(buffer, 24));
    header.status = static_cast<int32_t>(ntohl(loadAt<uint32_t>(buffer, 28)));
    if (header.magic != kProtocolMagic || header.version != kProtocolVersion ||
        header.payloadSize > kMaxPayloadSize) {
        return false;
    }
    outHeader = header;
    return true;
}

void ByteWriter::writeU8(uint8_t value) {
    buffer_.push_back(value);
}

void ByteWriter::writeU16(uint16_t value) {
    const uint16_t networkValue = htons(value);
    writeBytes(&networkValue, sizeof(networkValue));
}

void ByteWriter::writeU32(uint32_t value) {
    const uint32_t networkValue = htonl(value);
    writeBytes(&networkValue, sizeof(networkValue));
}

void ByteWriter::writeU64(uint64_t value) {
    const uint64_t networkValue = hostToNetwork64(value);
    writeBytes(&networkValue, sizeof(networkValue));
}

void ByteWriter::writeBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void ByteWriter::writeFixedString(const std::string& value, size_t fieldSize) {
    std::vector<uint8_t> field(fieldSize, 0);
    std::memcpy(field.data(), value.data(), std::min(value.size(), fieldSize - 1));
    writeBytes(field.data(), field.size());
}

bool ByteReader::reserve(size_t size) {
    if (!ok_ || buffer_.size() - position_ < size) {
        ok_ = false;
        return false;
    }
    return true;
}

uint8_t ByteReader::readU8() {
    if (!reserve(1)) {
        return 0;
    }
    return buffer_[position_++];
}

uint16_t ByteReader::readU16() {
    uint16_t value = 0;
    readBytes(&value, sizeof(value));
    return ntohs(value);
}

uint32_t ByteReader::readU32() {
    uint32_t value = 0;
    readBytes(&value, sizeof(value));
    return ntohl(value);
}

uint64_t ByteReader::readU64() {
    uint64_t value = 0;
    readBytes(&value, sizeof(value));
    return networkToHost64(value);
}

void ByteReader::readBytes(void* destination, size_t size) {
    if (!reserve(size)) {
        std::memset(destination, 0, size);
        return;
    }
    std::memcpy(destination, buffer_.data() + position_, size);
    position_ += size;
}

std::string ByteReader::readFixedString(size_t fieldSize) {
    std::vector<char> field(fieldSize + 1, '\0');
    readBytes(field.data(), fieldSize);
    return std::string(field.data());
}

}  // namespace dsm::network
