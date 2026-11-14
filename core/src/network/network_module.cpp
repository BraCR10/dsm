#include "network/network_module.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "common/log.h"

namespace dsm::network {

namespace {

constexpr const char* kModule = "network";
constexpr uint32_t kConnectTimeoutMs = 1000;
constexpr int kListenBacklog = 64;

}  // namespace

NetworkModule::NetworkModule(uint32_t selfNodeId) : selfNodeId_(selfNodeId) {}

NetworkModule::~NetworkModule() {
    stop();
}

void NetworkModule::registerHandler(MessageType type, MessageHandler handler) {
    handlers_[static_cast<uint16_t>(type)] = std::move(handler);
}

void NetworkModule::registerPeerAddress(uint32_t nodeId, const std::string& ip, uint16_t mainPort,
                                        uint16_t heartbeatPort) {
    sync::LockGuard guard(peersMutex_);
    peerAddresses_[nodeId] = PeerAddress{ip, mainPort, heartbeatPort};
}

int NetworkModule::openListener(uint16_t port) {
    const int listenerDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenerDescriptor < 0) {
        return -1;
    }
    const int enabled = 1;
    setsockopt(listenerDescriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listenerDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listenerDescriptor, kListenBacklog) != 0) {
        close(listenerDescriptor);
        return -1;
    }
    return listenerDescriptor;
}

DsmStatus NetworkModule::start(uint16_t mainPort, uint16_t heartbeatPort) {
    const int mainListener = openListener(mainPort);
    const int heartbeatListener = openListener(heartbeatPort);
    if (mainListener < 0 || heartbeatListener < 0) {
        common::logError(kModule, "cannot listen on ports %u and %u", mainPort, heartbeatPort);
        if (mainListener >= 0) {
            close(mainListener);
        }
        if (heartbeatListener >= 0) {
            close(heartbeatListener);
        }
        return DSM_ERR_NETWORK;
    }
    running_.store(true);
    sync::LockGuard guard(threadsMutex_);
    listenerDescriptors_ = {mainListener, heartbeatListener};
    threads_.emplace_back(&NetworkModule::acceptLoop, this, mainListener, Channel::Main);
    threads_.emplace_back(&NetworkModule::acceptLoop, this, heartbeatListener, Channel::Heartbeat);
    threads_.emplace_back(&NetworkModule::dispatchLoop, this);
    common::logInfo(kModule, "listening on main port %u and heartbeat port %u", mainPort, heartbeatPort);
    return DSM_OK;
}

void NetworkModule::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    std::vector<std::thread> threadsToJoin;
    {
        sync::LockGuard guard(threadsMutex_);
        for (int listenerDescriptor : listenerDescriptors_) {
            shutdown(listenerDescriptor, SHUT_RDWR);
            close(listenerDescriptor);
        }
        listenerDescriptors_.clear();
    }
    {
        sync::LockGuard guard(connectionsMutex_);
        for (const auto& connection : allConnections_) {
            connection->shutdownSocket();
        }
    }
    queueItems_.post();
    {
        sync::LockGuard guard(threadsMutex_);
        threadsToJoin.swap(threads_);
    }
    for (std::thread& thread : threadsToJoin) {
        thread.join();
    }
    sync::LockGuard guard(connectionsMutex_);
    outgoingConnections_.clear();
    allConnections_.clear();
}

void NetworkModule::acceptLoop(int listenerDescriptor, Channel channel) {
    while (running_.load()) {
        const int socketDescriptor = accept4(listenerDescriptor, nullptr, nullptr, SOCK_CLOEXEC);
        if (socketDescriptor < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        auto connection = std::make_shared<Connection>(socketDescriptor, channel);
        {
            sync::LockGuard guard(connectionsMutex_);
            allConnections_.push_back(connection);
        }
        startReader(connection);
    }
}

void NetworkModule::startReader(const std::shared_ptr<Connection>& connection) {
    sync::LockGuard guard(threadsMutex_);
    if (running_.load()) {
        threads_.emplace_back(&NetworkModule::readLoop, this, connection);
    }
}

void NetworkModule::readLoop(std::shared_ptr<Connection> connection) {
    Message message;
    while (running_.load() && connection->receive(message)) {
        messagesReceived_.fetch_add(1);
        if (message.header.sourceNode != kExternalClientNodeId) {
            connection->setRemoteNodeId(message.header.sourceNode);
        }
        if (isResponseType(message.type()) && completePendingRequest(message)) {
            continue;
        }
        {
            sync::LockGuard guard(queueMutex_);
            dispatchQueue_.push_back(IncomingMessage{std::move(message), connection});
        }
        queueItems_.post();
        message = Message{};
    }
    failPendingRequestsOn(connection.get());
}

void NetworkModule::dispatchLoop() {
    while (true) {
        queueItems_.wait();
        if (!running_.load()) {
            return;
        }
        IncomingMessage incoming;
        {
            sync::LockGuard guard(queueMutex_);
            if (dispatchQueue_.empty()) {
                continue;
            }
            incoming = std::move(dispatchQueue_.front());
            dispatchQueue_.pop_front();
        }
        const auto handler = handlers_.find(incoming.message.header.type);
        if (handler == handlers_.end()) {
            common::logError(kModule, "no handler for %s from node %u", messageTypeName(incoming.message.type()),
                             incoming.message.header.sourceNode);
            continue;
        }
        handler->second(incoming);
    }
}

bool NetworkModule::completePendingRequest(const Message& message) {
    std::shared_ptr<PendingRequest> pending;
    {
        sync::LockGuard guard(pendingMutex_);
        const auto found = pendingRequests_.find(message.header.requestId);
        if (found == pendingRequests_.end()) {
            return false;
        }
        pending = found->second;
        pendingRequests_.erase(found);
    }
    if (pending->onResponse) {
        pending->onResponse(DSM_OK, message);
        return true;
    }
    pending->response = message;
    pending->completed.post();
    return true;
}

void NetworkModule::failPendingRequestsOn(const Connection* connection) {
    std::vector<std::shared_ptr<PendingRequest>> failedRequests;
    {
        sync::LockGuard guard(pendingMutex_);
        for (auto entry = pendingRequests_.begin(); entry != pendingRequests_.end();) {
            if (entry->second->connection.get() == connection) {
                failedRequests.push_back(entry->second);
                entry = pendingRequests_.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    for (const auto& pending : failedRequests) {
        if (pending->onResponse) {
            pending->onResponse(DSM_ERR_NODE_UNAVAILABLE, Message{});
            continue;
        }
        pending->failed = true;
        pending->completed.post();
    }
}

std::shared_ptr<Connection> NetworkModule::connectionTo(uint32_t nodeId, Channel channel) {
    {
        sync::LockGuard guard(connectionsMutex_);
        const auto found = outgoingConnections_.find({nodeId, channel});
        if (found != outgoingConnections_.end() && found->second->isOpen()) {
            return found->second;
        }
    }
    PeerAddress address;
    {
        sync::LockGuard guard(peersMutex_);
        const auto found = peerAddresses_.find(nodeId);
        if (found == peerAddresses_.end()) {
            return nullptr;
        }
        address = found->second;
    }
    const uint16_t port = channel == Channel::Main ? address.mainPort : address.heartbeatPort;
    auto connection = Connection::connectTo(address.ip, port, channel, kConnectTimeoutMs);
    if (!connection) {
        return nullptr;
    }
    connection->setRemoteNodeId(nodeId);
    {
        sync::LockGuard guard(connectionsMutex_);
        outgoingConnections_[{nodeId, channel}] = connection;
        allConnections_.push_back(connection);
    }
    startReader(connection);
    return connection;
}

bool NetworkModule::sendOn(const std::shared_ptr<Connection>& connection, Message& message) {
    message.header.sourceNode = selfNodeId_;
    if (!connection->send(message)) {
        return false;
    }
    messagesSent_.fetch_add(1);
    return true;
}

bool NetworkModule::send(uint32_t nodeId, Channel channel, Message message) {
    auto connection = connectionTo(nodeId, channel);
    if (!connection) {
        return false;
    }
    message.header.destinationNode = nodeId;
    return sendOn(connection, message);
}

DsmStatus NetworkModule::request(uint32_t nodeId, Channel channel, Message message, uint32_t timeoutMs,
                                 Message& outResponse) {
    auto connection = connectionTo(nodeId, channel);
    if (!connection) {
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    const uint64_t requestId = nextRequestId_.fetch_add(1);
    auto pending = std::make_shared<PendingRequest>();
    pending->connection = connection;
    {
        sync::LockGuard guard(pendingMutex_);
        pendingRequests_[requestId] = pending;
    }
    message.header.requestId = requestId;
    message.header.destinationNode = nodeId;
    if (!sendOn(connection, message)) {
        sync::LockGuard guard(pendingMutex_);
        pendingRequests_.erase(requestId);
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    if (!pending->completed.waitFor(timeoutMs)) {
        sync::LockGuard guard(pendingMutex_);
        pendingRequests_.erase(requestId);
        return DSM_ERR_NETWORK;
    }
    if (pending->failed) {
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    outResponse = std::move(pending->response);
    return DSM_OK;
}

void NetworkModule::requestAsync(uint32_t nodeId, Channel channel, Message message, ResponseCallback onResponse) {
    auto connection = connectionTo(nodeId, channel);
    if (!connection) {
        onResponse(DSM_ERR_NODE_UNAVAILABLE, Message{});
        return;
    }
    const uint64_t requestId = nextRequestId_.fetch_add(1);
    auto pending = std::make_shared<PendingRequest>();
    pending->connection = connection;
    pending->onResponse = std::move(onResponse);
    {
        sync::LockGuard guard(pendingMutex_);
        pendingRequests_[requestId] = pending;
    }
    message.header.requestId = requestId;
    message.header.destinationNode = nodeId;
    if (!sendOn(connection, message)) {
        bool stillPending = false;
        {
            sync::LockGuard guard(pendingMutex_);
            stillPending = pendingRequests_.erase(requestId) > 0;
        }
        if (stillPending) {
            pending->onResponse(DSM_ERR_NODE_UNAVAILABLE, Message{});
        }
    }
}

void NetworkModule::reply(const IncomingMessage& incoming, Message response) {
    response.header.requestId = incoming.message.header.requestId;
    response.header.destinationNode = incoming.message.header.sourceNode;
    sendOn(incoming.connection, response);
}

void NetworkModule::closeConnectionsTo(uint32_t nodeId) {
    sync::LockGuard guard(connectionsMutex_);
    for (auto entry = outgoingConnections_.begin(); entry != outgoingConnections_.end();) {
        if (entry->first.first == nodeId) {
            entry->second->shutdownSocket();
            entry = outgoingConnections_.erase(entry);
        } else {
            ++entry;
        }
    }
}

}  // namespace dsm::network
