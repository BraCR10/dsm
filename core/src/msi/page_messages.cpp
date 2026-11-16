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

std::vector<uint8_t> encodePageData(const PageGrant& grant, const void* content, size_t contentSize) {
    ByteWriter writer;
    writer.writeU64(grant.globalPageId);
    writer.writeU8(static_cast<uint8_t>(grant.grantedState));
    writer.writeU64(grant.version);
    writer.writeBytes(content, contentSize);
    return writer.take();
}

std::optional<PageDataPayload> decodePageData(const std::vector<uint8_t>& bytes, size_t pageSize) {
    ByteReader reader(bytes);
    PageDataPayload payload;
    payload.grant.globalPageId = reader.readU64();
    const uint8_t grantedState = reader.readU8();
    payload.grant.version = reader.readU64();
    payload.content.resize(pageSize);
    reader.readBytes(payload.content.data(), pageSize);
    if (!reader.ok() || !reader.atEnd() || grantedState > MSI_MODIFIED) {
        return std::nullopt;
    }
    payload.grant.grantedState = static_cast<MsiState>(grantedState);
    return payload;
}

std::vector<uint8_t> encodePageVersion(const PageVersionPayload& payload) {
    ByteWriter writer;
    writer.writeU64(payload.globalPageId);
    writer.writeU64(payload.version);
    return writer.take();
}

std::optional<PageVersionPayload> decodePageVersion(const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes);
    PageVersionPayload payload;
    payload.globalPageId = reader.readU64();
    payload.version = reader.readU64();
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
