#include "memory/memory_messages.h"

#include "network/message.h"

namespace dsm::memory {

std::vector<uint8_t> encodeMemoryRequest(const MemoryRequestPayload& payload) {
    network::ByteWriter writer;
    writer.writeU64(payload.pageCount);
    writer.writeU32(payload.processId);
    return writer.take();
}

std::optional<MemoryRequestPayload> decodeMemoryRequest(const std::vector<uint8_t>& bytes) {
    network::ByteReader reader(bytes);
    MemoryRequestPayload payload;
    payload.pageCount = reader.readU64();
    payload.processId = reader.readU32();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return payload;
}

std::vector<uint8_t> encodeAddress(uint64_t address) {
    network::ByteWriter writer;
    writer.writeU64(address);
    return writer.take();
}

std::optional<uint64_t> decodeAddress(const std::vector<uint8_t>& bytes) {
    network::ByteReader reader(bytes);
    const uint64_t address = reader.readU64();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return address;
}

}  // namespace dsm::memory
