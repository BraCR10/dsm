#ifndef DSM_SYNC_MUTEX_COORDINATOR_H
#define DSM_SYNC_MUTEX_COORDINATOR_H

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

#include "dsm/dsm.h"
#include "sync/mutex.h"
#include "sync/sync_participant.h"

namespace dsm::sync {

// State of the distributed mutexes managed by this node. Completions always run after the
// internal lock is released, so they may send messages without holding the table.
class MutexCoordinator {
public:
    explicit MutexCoordinator(uint32_t selfNodeId) : selfNodeId_(selfNodeId) {}

    DsmStatus create(int32_t& outMutexId);
    void lock(uint32_t mutexId, const SyncParticipant& requester, SyncCompletion completion);
    DsmStatus unlock(uint32_t mutexId, const SyncParticipant& requester);
    DsmStatus destroy(uint32_t mutexId);
    void releaseProcess(uint32_t nodeId, uint32_t processId);

private:
    struct Waiter {
        SyncParticipant participant;
        SyncCompletion completion;
    };

    struct MutexState {
        std::optional<SyncParticipant> owner;
        std::deque<Waiter> waiters;
    };

    std::optional<Waiter> passOwnership(MutexState& state);

    const uint32_t selfNodeId_;
    Mutex tableMutex_;
    uint32_t nextCounter_ = 0;
    std::unordered_map<uint32_t, MutexState> mutexes_;
};

}  // namespace dsm::sync

#endif
