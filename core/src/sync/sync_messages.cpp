#include "sync/sync_messages.h"

#include "network/message.h"

namespace dsm::sync {

std::vector<uint8_t> encodeSyncRequest(const SyncRequestPayload& payload) {
    network::ByteWriter writer;
    writer.writeU32(payload.objectId);
    writer.writeU32(payload.processId);
    writer.writeU32(payload.threadId);
    return writer.take();
}

std::optional<SyncRequestPayload> decodeSyncRequest(const std::vector<uint8_t>& bytes) {
    network::ByteReader reader(bytes);
    SyncRequestPayload payload;
    payload.objectId = reader.readU32();
    payload.processId = reader.readU32();
    payload.threadId = reader.readU32();
    if (!reader.ok() || !reader.atEnd()) {
        return std::nullopt;
    }
    return payload;
}

}  // namespace dsm::sync
