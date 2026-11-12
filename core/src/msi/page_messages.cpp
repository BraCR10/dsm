#include "msi/page_messages.h"

namespace dsm::msi {

using network::AccessType;
using network::ByteReader;
using network::ByteWriter;

std::vector<uint8_t> encodePageRequest(const PageRequestPayload& payload) {
    ByteWriter writer;
    writer.writeU64(payload.globalPageId);
    writer.writeU8(static_cast<uint8_t>(payload.access));
    return writer.take();
}

std::optional<PageRequestPayload> decodePageRequest(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    PageRequestPayload payload;
    payload.globalPageId = reader.readU64();
    const uint8_t access = reader.readU8();
    if (!reader.ok() || !reader.atEnd() ||
        (access != static_cast<uint8_t>(AccessType::Read) && access != static_cast<uint8_t>(AccessType::Write))) {
        return std::nullopt;
    }
    payload.access = static_cast<AccessType>(access);
    return payload;
}

std::vector<uint8_t> encodePageData(uint64_t globalPageId, const void* content, size_t contentSize) {
    ByteWriter writer;
    writer.writeU64(globalPageId);
    writer.writeBytes(content, contentSize);
    return writer.take();
}

std::optional<PageDataPayload> decodePageData(const std::vector<uint8_t>& bytes, size_t pageSize) {
    ByteReader reader(bytes);
    PageDataPayload payload;
    payload.globalPageId = reader.readU64();
    payload.content.resize(pageSize);
    reader.readBytes(payload.content.data(), pageSize);
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return payload;
}

std::vector<uint8_t> encodePageId(uint64_t globalPageId) {
    ByteWriter writer;
    writer.writeU64(globalPageId);
    return writer.take();
}

std::optional<uint64_t> decodePageId(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    const uint64_t globalPageId = reader.readU64();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return globalPageId;
}

}  // namespace dsm::msi
