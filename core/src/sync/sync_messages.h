#ifndef DSM_SYNC_SYNC_MESSAGES_H
#define DSM_SYNC_SYNC_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

namespace dsm::sync {

// Body of every mutex and barrier message: the object plus the thread that acts on it. The
// node of the thread travels in the source_node field of the header.
struct SyncRequestPayload {
    uint32_t objectId = 0;
    uint32_t processId = 0;
    uint32_t threadId = 0;
};

std::vector<uint8_t> encodeSyncRequest(const SyncRequestPayload& payload);
std::optional<SyncRequestPayload> decodeSyncRequest(const std::vector<uint8_t>& bytes);

}  // namespace dsm::sync

#endif
