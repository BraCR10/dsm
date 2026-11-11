#ifndef DSM_NETWORK_MESSAGE_H
#define DSM_NETWORK_MESSAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dsm::network {

constexpr uint32_t kProtocolMagic = 0x44534D31;
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr uint32_t kMaxPayloadSize = 64u * 1024u * 1024u;
constexpr uint32_t kExternalClientNodeId = 0xFFFFFFFFu;

enum class Channel : uint8_t { Main, Heartbeat };

enum class MessageType : uint16_t {
    NodeJoin = 1,
    NodeJoinAck = 2,
    NodeLeave = 3,
    NodeDown = 4,
    Heartbeat = 5,
    HeartbeatProbe = 6,
    MemoryRequest = 10,
    MemoryGrant = 11,
    PageRequest = 20,
    PageData = 21,
    PageRecall = 22,
    Invalidate = 23,
    InvalidateAck = 24,
    NamePublish = 30,
    NameAck = 31,
    NameUnlink = 32,
    LockRequest = 40,
    LockGrant = 41,
    LockRelease = 42,
    BarrierArrive = 50,
    BarrierRelease = 51,
    StatsRequest = 60,
    StatsData = 61,
    PageTableRequest = 62,
    PageTableData = 63,
};

enum class AccessType : uint8_t { Read = 1, Write = 2 };

struct MessageHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint32_t sourceNode = 0;
    uint32_t destinationNode = 0;
    uint64_t requestId = 0;
    uint32_t payloadSize = 0;
    int32_t status = 0;
};

struct Message {
    MessageHeader header;
    std::vector<uint8_t> payload;

    MessageType type() const { return static_cast<MessageType>(header.type); }
};

Message makeMessage(MessageType type, std::vector<uint8_t> payload = {});
bool isResponseType(MessageType type);
const char* messageTypeName(MessageType type);

void encodeHeader(const MessageHeader& header, uint8_t (&buffer)[kHeaderSize]);
bool decodeHeader(const uint8_t (&buffer)[kHeaderSize], MessageHeader& outHeader);

class ByteWriter {
public:
    void writeU8(uint8_t value);
    void writeU16(uint16_t value);
    void writeU32(uint32_t value);
    void writeU64(uint64_t value);
    void writeBytes(const void* data, size_t size);
    void writeFixedString(const std::string& value, size_t fieldSize);
    std::vector<uint8_t> take() { return std::move(buffer_); }

private:
    std::vector<uint8_t> buffer_;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<uint8_t>& buffer) : buffer_(buffer) {}

    uint8_t readU8();
    uint16_t readU16();
    uint32_t readU32();
    uint64_t readU64();
    void readBytes(void* destination, size_t size);
    std::string readFixedString(size_t fieldSize);
    bool ok() const { return ok_; }
    bool atEnd() const { return position_ == buffer_.size(); }

private:
    bool reserve(size_t size);

    const std::vector<uint8_t>& buffer_;
    size_t position_ = 0;
    bool ok_ = true;
};

}  // namespace dsm::network

#endif
