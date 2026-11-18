#ifndef DSM_SYNC_SYNC_PARTICIPANT_H
#define DSM_SYNC_SYNC_PARTICIPANT_H

#include <cstdint>
#include <functional>

#include "dsm/dsm.h"

namespace dsm::sync {

constexpr uint32_t kSyncCounterBits = 16;
constexpr uint32_t kSyncCounterLimit = 1u << kSyncCounterBits;

struct SyncParticipant {
    uint32_t nodeId = 0;
    uint32_t processId = 0;
    uint32_t threadId = 0;

    bool operator==(const SyncParticipant& other) const {
        return nodeId == other.nodeId && processId == other.processId && threadId == other.threadId;
    }
    bool belongsTo(uint32_t otherNodeId, uint32_t otherProcessId) const {
        return nodeId == otherNodeId && processId == otherProcessId;
    }
};

using SyncCompletion = std::function<void(DsmStatus)>;

inline uint32_t managerOf(uint32_t objectId) {
    return objectId >> kSyncCounterBits;
}

inline uint32_t makeSyncObjectId(uint32_t managerNodeId, uint32_t localCounter) {
    return (managerNodeId << kSyncCounterBits) | localCounter;
}

}  // namespace dsm::sync

#endif
