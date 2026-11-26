#include "api/local_service.h"

#include <sys/socket.h>
#include <unistd.h>

#include "api/local_messages.h"
#include "common/log.h"
#include "network/unix_socket.h"
#include "stats/stats_messages.h"
#include "sync/sync_messages.h"

namespace dsm::api {

namespace {

constexpr const char* kModule = "api";
constexpr uint32_t kProtectionTimeoutMs = 4000;

using network::makeMessage;
using network::Message;
using network::MessageType;

MessageType syncAckTypeFor(MessageType requestType) {
    switch (requestType) {
        case MessageType::LocalMutexLock: return MessageType::LocalMutexLockAck;
        case MessageType::LocalMutexUnlock: return MessageType::LocalMutexUnlockAck;
        case MessageType::LocalMutexDestroy: return MessageType::LocalMutexDestroyAck;
        case MessageType::LocalBarrierWait: return MessageType::LocalBarrierWaitAck;
        default: return MessageType::LocalBarrierDestroyAck;
    }
}

}  // namespace

LocalService::LocalService(const settings::ClusterConfig& config, uint32_t selfNodeId, memory::MemoryModule& memory,
                           stats::StatsModule& stats, peers::PeersModule& peers, sync::DistributedSyncModule& sync)
    : config_(config), selfNodeId_(selfNodeId), memory_(memory), stats_(stats), peers_(peers), sync_(sync) {}

LocalService::~LocalService() {
    stop();
}

DsmStatus LocalService::start() {
    const std::string path = network::nodeSocketPath(selfNodeId_);
    listenerDescriptor_ = network::listenOnUnixSocket(path);
    if (listenerDescriptor_ < 0) {
        common::logError(kModule, common::fromSelf(), "cannot listen on %s", path.c_str());
        return DSM_ERR_NETWORK;
    }
    running_.store(true);
    acceptThread_ = std::thread(&LocalService::acceptLoop, this);
    common::logInfo(kModule, common::fromSelf(), "applications attach through %s", path.c_str());
    return DSM_OK;
}

void LocalService::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    shutdown(listenerDescriptor_, SHUT_RDWR);
    close(listenerDescriptor_);
    acceptThread_.join();
    unlink(network::nodeSocketPath(selfNodeId_).c_str());
    std::list<std::shared_ptr<Client>> clients;
    {
        sync::LockGuard guard(clientsMutex_);
        clients.swap(clients_);
    }
    for (const auto& client : clients) {
        client->channel->stop();
    }
}

void LocalService::acceptLoop() {
    while (running_.load()) {
        auto connection = network::acceptUnixConnection(listenerDescriptor_);
        if (!connection) {
            return;
        }
        removeClosedClients();
        auto client = std::make_shared<Client>();
        client->self = client;
        Client* clientPointer = client.get();
        client->channel = std::make_unique<network::LocalChannel>(
            connection, [this, clientPointer](const Message& request) { handleRequest(*clientPointer, request); });
        {
            sync::LockGuard guard(clientsMutex_);
            clients_.push_back(client);
        }
        client->channel->start([this, clientPointer] { onClientClosed(*clientPointer); });
    }
}

void LocalService::onClientClosed(Client& client) {
    client.closed.store(true);
    if (client.attached.load()) {
        common::logInfo(kModule, common::fromProcess(client.processId), "process detached");
        sync_.releaseProcess(client.processId);
        const uint64_t releasedPages = memory_.releaseProcess(client.processId);
        if (releasedPages > 0) {
            common::logInfo(kModule, common::fromProcess(client.processId),
                            "released %llu pages the process did not free",
                            static_cast<unsigned long long>(releasedPages));
        }
    }
}

void LocalService::removeClosedClients() {
    std::list<std::shared_ptr<Client>> closedClients;
    {
        sync::LockGuard guard(clientsMutex_);
        for (auto entry = clients_.begin(); entry != clients_.end();) {
            if ((*entry)->closed.load()) {
                closedClients.push_back(*entry);
                entry = clients_.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    for (const auto& client : closedClients) {
        client->channel->stop();
    }
}

std::vector<std::shared_ptr<LocalService::Client>> LocalService::openClients() {
    sync::LockGuard guard(clientsMutex_);
    std::vector<std::shared_ptr<Client>> clients;
    for (const auto& client : clients_) {
        if (client->attached.load() && !client->closed.load()) {
            clients.push_back(client);
        }
    }
    return clients;
}

void LocalService::restrictApplicationAccess(uint64_t pageId, int protection) {
    const auto payload = encodeProtection({pageId, static_cast<uint32_t>(protection)});
    for (const auto& client : openClients()) {
        Message response;
        client->channel->request(makeMessage(MessageType::LocalSetProtection, payload), kProtectionTimeoutMs,
                                 response);
    }
}

void LocalService::replyStatus(Client& client, const Message& request, MessageType type, DsmStatus status,
                               std::vector<uint8_t> payload) {
    Message response = makeMessage(type, std::move(payload));
    response.header.status = status;
    client.channel->reply(request, std::move(response));
}

void LocalService::handleRequest(Client& client, const Message& request) {
    if (request.type() == MessageType::LocalAttach) {
        handleAttach(client, request);
        return;
    }
    if (!client.attached.load()) {
        replyStatus(client, request, MessageType::LocalAttachAck, DSM_ERR_NOT_INITIALIZED);
        return;
    }
    switch (request.type()) {
        case MessageType::LocalMalloc: {
            const auto size = decodeValue(request.payload);
            DsmStatus status = size ? DSM_OK : DSM_ERR_INVALID_MESSAGE;
            const uintptr_t address = size ? memory_.allocate(*size, client.processId, status) : 0;
            replyStatus(client, request, MessageType::LocalMallocAck, status, encodeValue(address));
            return;
        }
        case MessageType::LocalFree: {
            const auto address = decodeValue(request.payload);
            uint64_t pageCount = 0;
            const DsmStatus status = address ? memory_.release(*address, pageCount) : DSM_ERR_INVALID_MESSAGE;
            replyStatus(client, request, MessageType::LocalFreeAck, status, encodeValue(pageCount));
            return;
        }
        case MessageType::LocalFault:
            handleFault(client, request);
            return;
        case MessageType::LocalPageStatus: {
            const auto address = decodeValue(request.payload);
            MsiState state = MSI_INVALID;
            const DsmStatus status = address ? memory_.pageState(*address, state) : DSM_ERR_INVALID_MESSAGE;
            replyStatus(client, request, MessageType::LocalPageStatusAck, status, encodeValue(state));
            return;
        }
        case MessageType::LocalNodeTable:
            replyStatus(client, request, MessageType::LocalNodeTableAck, DSM_OK,
                        encodeNodeTable(peers_.nodeTable().toNodeInfo()));
            return;
        case MessageType::StatsRequest:
            handleStats(client, request);
            return;
        case MessageType::PageTableRequest:
            handlePageTable(client, request);
            return;
        case MessageType::LocalMutexInit:
        case MessageType::LocalBarrierInit:
            handleSyncCreate(client, request);
            return;
        case MessageType::LocalMutexLock:
        case MessageType::LocalMutexUnlock:
        case MessageType::LocalMutexDestroy:
        case MessageType::LocalBarrierWait:
        case MessageType::LocalBarrierDestroy:
            handleSyncOperation(client, request);
            return;
        default:
            common::logError(kModule, common::fromProcess(client.processId), "unexpected %s",
                             network::messageTypeName(request.type()));
    }
}

void LocalService::handleAttach(Client& client, const Message& request) {
    const auto attach = decodeAttach(request.payload);
    if (!attach) {
        replyStatus(client, request, MessageType::LocalAttachAck, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    if (attach->configHash != config_.configHash) {
        replyStatus(client, request, MessageType::LocalAttachAck, DSM_ERR_CONFIG_MISMATCH);
        return;
    }
    client.processId = attach->processId;
    client.attached.store(true);
    common::logInfo(kModule, common::fromProcess(client.processId), "process attached");
    replyStatus(client, request, MessageType::LocalAttachAck, DSM_OK, encodeValue(selfNodeId_));
}

void LocalService::handleFault(Client& client, const Message& request) {
    const auto fault = decodeFault(request.payload);
    if (!fault) {
        replyStatus(client, request, MessageType::LocalFaultAck, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    memory_.resolveApplicationFault(fault->address, fault->isWrite,
                                    [this, &client, &request](const memory::FaultResolution& resolution) {
                                        replyStatus(client, request, MessageType::LocalFaultAck, resolution.status,
                                                    encodeProtection({resolution.pageId,
                                                                      static_cast<uint32_t>(resolution.protection)}));
                                    });
}

void LocalService::handleStats(Client& client, const Message& request) {
    const auto requestedNode = request.payload.empty() ? std::optional<uint64_t>(selfNodeId_)
                                                       : decodeValue(request.payload);
    if (!requestedNode) {
        replyStatus(client, request, MessageType::StatsData, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    DsmStats nodeStats{};
    DsmStatus status = DSM_OK;
    if (*requestedNode == selfNodeId_) {
        nodeStats = stats_.localStats();
    } else {
        status = stats_.remoteStats(static_cast<uint32_t>(*requestedNode), nodeStats);
    }
    replyStatus(client, request, MessageType::StatsData, status, stats::encodeStats(nodeStats));
}

void LocalService::handlePageTable(Client& client, const Message& request) {
    const auto requestedNode = request.payload.empty() ? std::optional<uint64_t>(selfNodeId_)
                                                       : decodeValue(request.payload);
    if (!requestedNode) {
        replyStatus(client, request, MessageType::PageTableData, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    std::vector<PageTableEntry> entries;
    DsmStatus status = DSM_OK;
    if (*requestedNode == selfNodeId_) {
        entries = memory_.pageTableView();
    } else {
        status = stats_.remotePageTable(static_cast<uint32_t>(*requestedNode), entries);
    }
    replyStatus(client, request, MessageType::PageTableData, status, stats::encodePageTable(entries));
}

void LocalService::handleSyncCreate(Client& client, const Message& request) {
    int32_t objectId = 0;
    if (request.type() == MessageType::LocalMutexInit) {
        const DsmStatus status = sync_.createMutex(objectId);
        replyStatus(client, request, MessageType::LocalMutexInitAck, status, encodeValue(static_cast<uint32_t>(objectId)));
        return;
    }
    const auto participantCount = decodeValue(request.payload);
    DsmStatus status = DSM_ERR_INVALID_ARGUMENT;
    if (!participantCount) {
        status = DSM_ERR_INVALID_MESSAGE;
    } else if (*participantCount <= static_cast<uint64_t>(INT32_MAX)) {
        status = sync_.createBarrier(static_cast<int32_t>(*participantCount), objectId);
    }
    replyStatus(client, request, MessageType::LocalBarrierInitAck, status, encodeValue(static_cast<uint32_t>(objectId)));
}

void LocalService::handleSyncOperation(Client& client, const Message& request) {
    const auto payload = sync::decodeSyncRequest(request.payload);
    const MessageType ackType = syncAckTypeFor(request.type());
    if (!payload) {
        replyStatus(client, request, ackType, DSM_ERR_INVALID_MESSAGE);
        return;
    }
    const sync::SyncParticipant participant{selfNodeId_, client.processId, payload->threadId};
    switch (request.type()) {
        case MessageType::LocalMutexLock:
            sync_.lockMutex(payload->objectId, participant,
                            replyOrUnlockWhenGranted(client, request, payload->objectId, participant));
            return;
        case MessageType::LocalMutexUnlock:
            sync_.unlockMutex(payload->objectId, participant, replyWhenDone(client, request, ackType));
            return;
        case MessageType::LocalMutexDestroy:
            sync_.destroyMutex(payload->objectId, replyWhenDone(client, request, ackType));
            return;
        case MessageType::LocalBarrierWait:
            sync_.waitBarrier(payload->objectId, participant, replyWhenDone(client, request, ackType));
            return;
        default:
            sync_.destroyBarrier(payload->objectId, replyWhenDone(client, request, ackType));
    }
}

sync::SyncCompletion LocalService::replyWhenDone(Client& client, const Message& request, MessageType type) {
    return [this, weakClient = client.self, request, type](DsmStatus status) {
        const auto liveClient = weakClient.lock();
        if (liveClient && !liveClient->closed.load()) {
            replyStatus(*liveClient, request, type, status);
        }
    };
}

sync::SyncCompletion LocalService::replyOrUnlockWhenGranted(Client& client, const Message& request, uint32_t mutexId,
                                                            const sync::SyncParticipant& requester) {
    return [this, weakClient = client.self, request, mutexId, requester](DsmStatus status) {
        const auto liveClient = weakClient.lock();
        if (liveClient && !liveClient->closed.load()) {
            replyStatus(*liveClient, request, MessageType::LocalMutexLockAck, status);
            return;
        }
        if (status == DSM_OK) {
            common::logInfo(kModule, common::fromProcess(requester.processId),
                            "releasing mutex 0x%x granted after the process closed", mutexId);
            sync_.unlockMutex(mutexId, requester, [](DsmStatus) {});
        }
    };
}

}  // namespace dsm::api
