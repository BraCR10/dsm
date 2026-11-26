#include "memory/memory_messages.h"

#include "network/message.h"

namespace dsm::memory {

std::vector<uint8_t> encodeStorageRequest(const StorageRequestPayload& payload) {
    network::ByteWriter writer;
    writer.writeU64(payload.firstPageId);
    writer.writeU64(payload.pageCount);
    writer.writeU64(payload.version);
    return writer.take();
}

std::optional<StorageRequestPayload> decodeStorageRequest(const std::vector<uint8_t>& bytes) {
    network::ByteReader reader(bytes);
    StorageRequestPayload payload;
    payload.firstPageId = reader.readU64();
    payload.pageCount = reader.readU64();
    payload.version = reader.readU64();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return payload;
}

std::vector<uint8_t> encodeStorageRelease(const StorageReleasePayload& payload) {
    network::ByteWriter writer;
    writer.writeU64(payload.firstPageId);
    writer.writeU64(payload.pageCount);
    return writer.take();
}

std::optional<StorageReleasePayload> decodeStorageRelease(const std::vector<uint8_t>& bytes) {
    network::ByteReader reader(bytes);
    StorageReleasePayload payload;
    payload.firstPageId = reader.readU64();
    payload.pageCount = reader.readU64();
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
