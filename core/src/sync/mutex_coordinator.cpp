#include "sync/mutex_coordinator.h"

#include <utility>
#include <vector>

namespace dsm::sync {

DsmStatus MutexCoordinator::create(int32_t& outMutexId) {
    LockGuard guard(tableMutex_);
    if (nextCounter_ >= kSyncCounterLimit) {
        return DSM_ERR_INTERNAL;
    }
    const uint32_t mutexId = makeSyncObjectId(selfNodeId_, nextCounter_++);
    mutexes_[mutexId] = MutexState{};
    outMutexId = static_cast<int32_t>(mutexId);
    return DSM_OK;
}

void MutexCoordinator::lock(uint32_t mutexId, const SyncParticipant& requester, SyncCompletion completion) {
    DsmStatus immediateStatus = DSM_OK;
    {
        LockGuard guard(tableMutex_);
        const auto found = mutexes_.find(mutexId);
        if (found == mutexes_.end()) {
            immediateStatus = DSM_ERR_NOT_FOUND;
        } else if (found->second.ownerNodeLost) {
            immediateStatus = DSM_ERR_NODE_UNAVAILABLE;
        } else if (found->second.owner && *found->second.owner == requester) {
            immediateStatus = DSM_ERR_DEADLOCK;
        } else if (found->second.owner) {
            found->second.waiters.push_back(Waiter{requester, std::move(completion)});
            return;
        } else {
            found->second.owner = requester;
        }
    }
    completion(immediateStatus);
}

std::optional<MutexCoordinator::Waiter> MutexCoordinator::passOwnership(MutexState& state) {
    if (state.waiters.empty()) {
        state.owner.reset();
        return std::nullopt;
    }
    Waiter next = std::move(state.waiters.front());
    state.waiters.pop_front();
    state.owner = next.participant;
    return next;
}

DsmStatus MutexCoordinator::unlock(uint32_t mutexId, const SyncParticipant& requester) {
    std::optional<Waiter> granted;
    {
        LockGuard guard(tableMutex_);
        const auto found = mutexes_.find(mutexId);
        if (found == mutexes_.end()) {
            return DSM_ERR_NOT_FOUND;
        }
        if (!found->second.owner || !(*found->second.owner == requester)) {
            return DSM_ERR_NOT_OWNER;
        }
        granted = passOwnership(found->second);
    }
    if (granted) {
        granted->completion(DSM_OK);
    }
    return DSM_OK;
}

DsmStatus MutexCoordinator::destroy(uint32_t mutexId) {
    LockGuard guard(tableMutex_);
    const auto found = mutexes_.find(mutexId);
    if (found == mutexes_.end()) {
        return DSM_ERR_NOT_FOUND;
    }
    if (found->second.owner || !found->second.waiters.empty()) {
        return DSM_ERR_BUSY;
    }
    mutexes_.erase(found);
    return DSM_OK;
}

void MutexCoordinator::releaseProcess(uint32_t nodeId, uint32_t processId) {
    std::vector<Waiter> granted;
    {
        LockGuard guard(tableMutex_);
        for (auto& [mutexId, state] : mutexes_) {
            std::deque<Waiter> remaining;
            for (Waiter& waiter : state.waiters) {
                if (!waiter.participant.belongsTo(nodeId, processId)) {
                    remaining.push_back(std::move(waiter));
                }
            }
            state.waiters.swap(remaining);
            if (state.owner && state.owner->belongsTo(nodeId, processId)) {
                std::optional<Waiter> next = passOwnership(state);
                if (next) {
                    granted.push_back(std::move(*next));
                }
            }
        }
    }
    for (Waiter& waiter : granted) {
        waiter.completion(DSM_OK);
    }
}

void MutexCoordinator::releaseNode(uint32_t nodeId) {
    std::vector<Waiter> failed;
    {
        LockGuard guard(tableMutex_);
        for (auto& [mutexId, state] : mutexes_) {
            std::deque<Waiter> remaining;
            for (Waiter& waiter : state.waiters) {
                if (waiter.participant.nodeId != nodeId) {
                    remaining.push_back(std::move(waiter));
                }
            }
            state.waiters.swap(remaining);
            if (state.owner && state.owner->nodeId == nodeId) {
                state.owner.reset();
                state.ownerNodeLost = true;
                for (Waiter& waiter : state.waiters) {
                    failed.push_back(std::move(waiter));
                }
                state.waiters.clear();
            }
        }
    }
    for (Waiter& waiter : failed) {
        waiter.completion(DSM_ERR_NODE_UNAVAILABLE);
    }
}

}  // namespace dsm::sync
