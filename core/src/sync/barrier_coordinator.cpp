#include "sync/barrier_coordinator.h"

#include <utility>

namespace dsm::sync {

DsmStatus BarrierCoordinator::create(int32_t participantCount, int32_t& outBarrierId) {
    if (participantCount <= 0) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    LockGuard guard(tableMutex_);
    if (nextCounter_ >= kSyncCounterLimit) {
        return DSM_ERR_INTERNAL;
    }
    const uint32_t barrierId = makeSyncObjectId(selfNodeId_, nextCounter_++);
    barriers_[barrierId] = BarrierState{static_cast<uint32_t>(participantCount), {}};
    outBarrierId = static_cast<int32_t>(barrierId);
    return DSM_OK;
}

void BarrierCoordinator::arrive(uint32_t barrierId, const SyncParticipant& participant, SyncCompletion completion) {
    std::vector<Waiter> released;
    bool barrierExists = true;
    {
        LockGuard guard(tableMutex_);
        const auto found = barriers_.find(barrierId);
        if (found == barriers_.end()) {
            barrierExists = false;
        } else {
            BarrierState& state = found->second;
            state.waiters.push_back(Waiter{participant, std::move(completion)});
            if (state.waiters.size() < state.participantCount) {
                return;
            }
            released.swap(state.waiters);
        }
    }
    if (!barrierExists) {
        completion(DSM_ERR_NOT_FOUND);
        return;
    }
    for (Waiter& waiter : released) {
        waiter.completion(DSM_OK);
    }
}

DsmStatus BarrierCoordinator::destroy(uint32_t barrierId) {
    LockGuard guard(tableMutex_);
    const auto found = barriers_.find(barrierId);
    if (found == barriers_.end()) {
        return DSM_ERR_NOT_FOUND;
    }
    if (!found->second.waiters.empty()) {
        return DSM_ERR_BUSY;
    }
    barriers_.erase(found);
    return DSM_OK;
}

void BarrierCoordinator::releaseProcess(uint32_t nodeId, uint32_t processId) {
    LockGuard guard(tableMutex_);
    for (auto& [barrierId, state] : barriers_) {
        std::vector<Waiter> remaining;
        for (Waiter& waiter : state.waiters) {
            if (!waiter.participant.belongsTo(nodeId, processId)) {
                remaining.push_back(std::move(waiter));
            }
        }
        state.waiters.swap(remaining);
    }
}

void BarrierCoordinator::releaseNode(uint32_t nodeId) {
    std::vector<Waiter> failed;
    {
        LockGuard guard(tableMutex_);
        for (auto& [barrierId, state] : barriers_) {
            for (Waiter& waiter : state.waiters) {
                if (waiter.participant.nodeId != nodeId) {
                    failed.push_back(std::move(waiter));
                }
            }
            state.waiters.clear();
        }
    }
    for (Waiter& waiter : failed) {
        waiter.completion(DSM_ERR_NODE_UNAVAILABLE);
    }
}

}  // namespace dsm::sync
