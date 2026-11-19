#include "sync/distributed_sync_module.h"

#include <utility>

#include "common/log.h"
#include "sync/sync_messages.h"

namespace dsm::sync {

namespace {

constexpr const char* kModule = "sync";

using network::Channel;
using network::IncomingMessage;
using network::makeMessage;
using network::Message;
using network::MessageType;

std::optional<SyncParticipant> participantFrom(const IncomingMessage& incoming, uint32_t& outObjectId) {
    const auto payload = decodeSyncRequest(incoming.message.payload);
    if (!payload) {
        return std::nullopt;
    }
    outObjectId = payload->objectId;
    return SyncParticipant{incoming.message.header.sourceNode, payload->processId, payload->threadId};
}

}  // namespace

DistributedSyncModule::DistributedSyncModule(uint32_t selfNodeId, network::NetworkModule& network)
    : selfNodeId_(selfNodeId), network_(network), mutexes_(selfNodeId), barriers_(selfNodeId) {}

void DistributedSyncModule::registerHandlers() {
    network_.registerHandler(MessageType::LockRequest, [this](const IncomingMessage& incoming) {
        handleLockRequest(incoming);
    });
    network_.registerHandler(MessageType::LockRelease, [this](const IncomingMessage& incoming) {
        handleLockRelease(incoming);
    });
    network_.registerHandler(MessageType::LockDestroy, [this](const IncomingMessage& incoming) {
        handleLockDestroy(incoming);
    });
    network_.registerHandler(MessageType::BarrierArrive, [this](const IncomingMessage& incoming) {
        handleBarrierArrive(incoming);
    });
    network_.registerHandler(MessageType::BarrierDestroy, [this](const IncomingMessage& incoming) {
        handleBarrierDestroy(incoming);
    });
    network_.registerHandler(MessageType::SyncReleaseProcess, [this](const IncomingMessage& incoming) {
        handleReleaseProcess(incoming);
    });
}

DsmStatus DistributedSyncModule::createMutex(int32_t& outMutexId) {
    return mutexes_.create(outMutexId);
}

void DistributedSyncModule::lockMutex(uint32_t mutexId, const SyncParticipant& requester, SyncCompletion completion) {
    if (isManagedHere(mutexId)) {
        mutexes_.lock(mutexId, requester, std::move(completion));
        return;
    }
    rememberRemoteManager(requester.processId, managerOf(mutexId));
    sendToManager(MessageType::LockRequest, mutexId, requester, std::move(completion));
}

void DistributedSyncModule::unlockMutex(uint32_t mutexId, const SyncParticipant& requester,
                                        SyncCompletion completion) {
    if (isManagedHere(mutexId)) {
        completion(mutexes_.unlock(mutexId, requester));
        return;
    }
    sendToManager(MessageType::LockRelease, mutexId, requester, std::move(completion));
}

void DistributedSyncModule::destroyMutex(uint32_t mutexId, SyncCompletion completion) {
    if (isManagedHere(mutexId)) {
        completion(mutexes_.destroy(mutexId));
        return;
    }
    sendToManager(MessageType::LockDestroy, mutexId, SyncParticipant{selfNodeId_, 0, 0}, std::move(completion));
}

DsmStatus DistributedSyncModule::createBarrier(int32_t participantCount, int32_t& outBarrierId) {
    return barriers_.create(participantCount, outBarrierId);
}

void DistributedSyncModule::waitBarrier(uint32_t barrierId, const SyncParticipant& participant,
                                        SyncCompletion completion) {
    if (isManagedHere(barrierId)) {
        barriers_.arrive(barrierId, participant, std::move(completion));
        return;
    }
    rememberRemoteManager(participant.processId, managerOf(barrierId));
    sendToManager(MessageType::BarrierArrive, barrierId, participant, std::move(completion));
}

void DistributedSyncModule::destroyBarrier(uint32_t barrierId, SyncCompletion completion) {
    if (isManagedHere(barrierId)) {
        completion(barriers_.destroy(barrierId));
        return;
    }
    sendToManager(MessageType::BarrierDestroy, barrierId, SyncParticipant{selfNodeId_, 0, 0},
                  std::move(completion));
}

void DistributedSyncModule::releaseProcess(uint32_t processId) {
    mutexes_.releaseProcess(selfNodeId_, processId);
    barriers_.releaseProcess(selfNodeId_, processId);
    for (uint32_t managerNodeId : takeRemoteManagersOf(processId)) {
        common::logInfo(kModule, common::fromProcess(processId),
                        "asking node %u to release the mutexes and barrier waits of the closed process", managerNodeId);
        sendToNode(managerNodeId, MessageType::SyncReleaseProcess, 0, SyncParticipant{selfNodeId_, processId, 0},
                   [](DsmStatus) {});
    }
}

void DistributedSyncModule::releaseNode(uint32_t nodeId) {
    mutexes_.releaseNode(nodeId);
    barriers_.releaseNode(nodeId);
}

void DistributedSyncModule::sendToManager(MessageType type, uint32_t objectId, const SyncParticipant& participant,
                                          SyncCompletion completion) {
    sendToNode(managerOf(objectId), type, objectId, participant, std::move(completion));
}

void DistributedSyncModule::sendToNode(uint32_t nodeId, MessageType type, uint32_t objectId,
                                       const SyncParticipant& participant, SyncCompletion completion) {
    Message request = makeMessage(type, encodeSyncRequest({objectId, participant.processId, participant.threadId}));
    network_.requestAsync(nodeId, Channel::Main, std::move(request),
                          [completion = std::move(completion)](DsmStatus status, const Message& response) {
                              completion(status == DSM_OK ? static_cast<DsmStatus>(response.header.status)
                                                          : status);
                          });
}

void DistributedSyncModule::rememberRemoteManager(uint32_t processId, uint32_t managerNodeId) {
    LockGuard guard(remoteManagersMutex_);
    remoteManagersByProcess_[processId].insert(managerNodeId);
}

std::set<uint32_t> DistributedSyncModule::takeRemoteManagersOf(uint32_t processId) {
    LockGuard guard(remoteManagersMutex_);
    const auto found = remoteManagersByProcess_.find(processId);
    if (found == remoteManagersByProcess_.end()) {
        return {};
    }
    std::set<uint32_t> managers = std::move(found->second);
    remoteManagersByProcess_.erase(found);
    return managers;
}

void DistributedSyncModule::replyStatus(const IncomingMessage& incoming, MessageType type, DsmStatus status) {
    Message response = makeMessage(type);
    response.header.status = status;
    network_.reply(incoming, std::move(response));
}

void DistributedSyncModule::handleLockRequest(const IncomingMessage& incoming) {
    uint32_t mutexId = 0;
    const auto requester = participantFrom(incoming, mutexId);
    if (!requester) {
        replyStatus(incoming, MessageType::LockGrant, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    mutexes_.lock(mutexId, *requester,
                  [this, incoming](DsmStatus status) { replyStatus(incoming, MessageType::LockGrant, status); });
}

void DistributedSyncModule::handleLockRelease(const IncomingMessage& incoming) {
    uint32_t mutexId = 0;
    const auto requester = participantFrom(incoming, mutexId);
    replyStatus(incoming, MessageType::LockReleaseAck,
                requester ? mutexes_.unlock(mutexId, *requester) : DSM_ERR_INVALID_MESSAGE);
}

void DistributedSyncModule::handleLockDestroy(const IncomingMessage& incoming) {
    uint32_t mutexId = 0;
    const auto requester = participantFrom(incoming, mutexId);
    replyStatus(incoming, MessageType::LockDestroyAck, requester ? mutexes_.destroy(mutexId) : DSM_ERR_INVALID_MESSAGE);
}

void DistributedSyncModule::handleBarrierArrive(const IncomingMessage& incoming) {
    uint32_t barrierId = 0;
    const auto participant = participantFrom(incoming, barrierId);
    if (!participant) {
        replyStatus(incoming, MessageType::BarrierRelease, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    barriers_.arrive(barrierId, *participant, [this, incoming](DsmStatus status) {
        replyStatus(incoming, MessageType::BarrierRelease, status);
    });
}

void DistributedSyncModule::handleBarrierDestroy(const IncomingMessage& incoming) {
    uint32_t barrierId = 0;
    const auto requester = participantFrom(incoming, barrierId);
    replyStatus(incoming, MessageType::BarrierDestroyAck,
                requester ? barriers_.destroy(barrierId) : DSM_ERR_INVALID_MESSAGE);
}

void DistributedSyncModule::handleReleaseProcess(const IncomingMessage& incoming) {
    uint32_t unusedObjectId = 0;
    const auto closedProcess = participantFrom(incoming, unusedObjectId);
    if (!closedProcess) {
        replyStatus(incoming, MessageType::SyncReleaseProcessAck, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    mutexes_.releaseProcess(closedProcess->nodeId, closedProcess->processId);
    barriers_.releaseProcess(closedProcess->nodeId, closedProcess->processId);
    replyStatus(incoming, MessageType::SyncReleaseProcessAck, DSM_OK);
}

}  // namespace dsm::sync
