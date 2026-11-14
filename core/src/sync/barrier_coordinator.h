#ifndef DSM_SYNC_BARRIER_COORDINATOR_H
#define DSM_SYNC_BARRIER_COORDINATOR_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dsm/dsm.h"
#include "sync/mutex.h"
#include "sync/sync_participant.h"

namespace dsm::sync {

// State of the distributed barriers managed by this node. A barrier releases every waiter when
// the last participant arrives and then starts counting again, so it can be reused.
class BarrierCoordinator {
public:
    explicit BarrierCoordinator(uint32_t selfNodeId) : selfNodeId_(selfNodeId) {}

    DsmStatus create(int32_t participantCount, int32_t& outBarrierId);
    void arrive(uint32_t barrierId, const SyncParticipant& participant, SyncCompletion completion);
    DsmStatus destroy(uint32_t barrierId);
    void releaseProcess(uint32_t nodeId, uint32_t processId);

private:
    struct Waiter {
        SyncParticipant participant;
        SyncCompletion completion;
    };

    struct BarrierState {
        uint32_t participantCount = 0;
        std::vector<Waiter> waiters;
    };

    const uint32_t selfNodeId_;
    Mutex tableMutex_;
    uint32_t nextCounter_ = 0;
    std::unordered_map<uint32_t, BarrierState> barriers_;
};

}  // namespace dsm::sync

#endif
