#ifndef DSM_SYNC_DISTRIBUTED_SYNC_MODULE_H
#define DSM_SYNC_DISTRIBUTED_SYNC_MODULE_H

#include <cstdint>
#include <set>
#include <unordered_map>

#include "dsm/dsm.h"
#include "network/network_module.h"
#include "sync/barrier_coordinator.h"
#include "sync/mutex.h"
#include "sync/mutex_coordinator.h"
#include "sync/sync_participant.h"

namespace dsm::sync {

class DistributedSyncModule {
public:
    DistributedSyncModule(uint32_t selfNodeId, network::NetworkModule& network);
    DistributedSyncModule(const DistributedSyncModule&) = delete;
    DistributedSyncModule& operator=(const DistributedSyncModule&) = delete;

    void registerHandlers();

    DsmStatus createMutex(int32_t& outMutexId);
    void lockMutex(uint32_t mutexId, const SyncParticipant& requester, SyncCompletion completion);
    void unlockMutex(uint32_t mutexId, const SyncParticipant& requester, SyncCompletion completion);
    void destroyMutex(uint32_t mutexId, SyncCompletion completion);

    DsmStatus createBarrier(int32_t participantCount, int32_t& outBarrierId);
    void waitBarrier(uint32_t barrierId, const SyncParticipant& participant, SyncCompletion completion);
    void destroyBarrier(uint32_t barrierId, SyncCompletion completion);

    void releaseProcess(uint32_t processId);

private:
    bool isManagedHere(uint32_t objectId) const { return managerOf(objectId) == selfNodeId_; }
    void sendToManager(network::MessageType type, uint32_t objectId, const SyncParticipant& participant,
                       SyncCompletion completion);
    void sendToNode(uint32_t nodeId, network::MessageType type, uint32_t objectId, const SyncParticipant& participant,
                    SyncCompletion completion);
    void rememberRemoteManager(uint32_t processId, uint32_t managerNodeId);
    std::set<uint32_t> takeRemoteManagersOf(uint32_t processId);

    void handleLockRequest(const network::IncomingMessage& incoming);
    void handleLockRelease(const network::IncomingMessage& incoming);
    void handleLockDestroy(const network::IncomingMessage& incoming);
    void handleBarrierArrive(const network::IncomingMessage& incoming);
    void handleBarrierDestroy(const network::IncomingMessage& incoming);
    void handleReleaseProcess(const network::IncomingMessage& incoming);
    void replyStatus(const network::IncomingMessage& incoming, network::MessageType type, DsmStatus status);

    const uint32_t selfNodeId_;
    network::NetworkModule& network_;
    MutexCoordinator mutexes_;
    BarrierCoordinator barriers_;
    Mutex remoteManagersMutex_;
    std::unordered_map<uint32_t, std::set<uint32_t>> remoteManagersByProcess_;
};

}  // namespace dsm::sync

#endif
