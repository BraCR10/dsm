#ifndef DSM_SYNC_SYNC_MESSAGES_H
#define DSM_SYNC_SYNC_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

namespace dsm::sync {

struct SyncRequestPayload {
    uint32_t objectId = 0;
    uint32_t processId = 0;
    uint32_t threadId = 0;
};

std::vector<uint8_t> encodeSyncRequest(const SyncRequestPayload& payload);
std::optional<SyncRequestPayload> decodeSyncRequest(const std::vector<uint8_t>& bytes);

}  // namespace dsm::sync

#endif
