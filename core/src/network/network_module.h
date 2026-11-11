#ifndef DSM_NETWORK_NETWORK_MODULE_H
#define DSM_NETWORK_NETWORK_MODULE_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dsm/dsm.h"
#include "network/connection.h"
#include "network/message.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"

namespace dsm::network {

struct IncomingMessage {
    Message message;
    std::shared_ptr<Connection> connection;
};

using MessageHandler = std::function<void(const IncomingMessage&)>;

class NetworkModule {
public:
    explicit NetworkModule(uint32_t selfNodeId);
    ~NetworkModule();
    NetworkModule(const NetworkModule&) = delete;
    NetworkModule& operator=(const NetworkModule&) = delete;

    void registerHandler(MessageType type, MessageHandler handler);
    void registerPeerAddress(uint32_t nodeId, const std::string& ip, uint16_t mainPort, uint16_t heartbeatPort);

    DsmStatus start(uint16_t mainPort, uint16_t heartbeatPort);
    void stop();

    bool send(uint32_t nodeId, Channel channel, Message message);
    DsmStatus request(uint32_t nodeId, Channel channel, Message message, uint32_t timeoutMs, Message& outResponse);
    void reply(const IncomingMessage& incoming, Message response);
    void closeConnectionsTo(uint32_t nodeId);

    uint64_t messagesSent() const { return messagesSent_.load(); }
    uint64_t messagesReceived() const { return messagesReceived_.load(); }

private:
    struct PeerAddress {
        std::string ip;
        uint16_t mainPort = 0;
        uint16_t heartbeatPort = 0;
    };

    struct PendingRequest {
        sync::Semaphore completed;
        Message response;
        std::shared_ptr<Connection> connection;
        bool failed = false;
    };

    using ConnectionKey = std::pair<uint32_t, Channel>;

    int openListener(uint16_t port);
    void acceptLoop(int listenerDescriptor, Channel channel);
    void readLoop(std::shared_ptr<Connection> connection);
    void dispatchLoop();
    void startReader(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<Connection> connectionTo(uint32_t nodeId, Channel channel);
    bool completePendingRequest(const Message& message);
    void failPendingRequestsOn(const Connection* connection);
    bool sendOn(const std::shared_ptr<Connection>& connection, Message& message);

    const uint32_t selfNodeId_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> nextRequestId_{1};
    std::atomic<uint64_t> messagesSent_{0};
    std::atomic<uint64_t> messagesReceived_{0};

    std::unordered_map<uint16_t, MessageHandler> handlers_;

    sync::Mutex peersMutex_;
    std::unordered_map<uint32_t, PeerAddress> peerAddresses_;

    sync::Mutex connectionsMutex_;
    std::map<ConnectionKey, std::shared_ptr<Connection>> outgoingConnections_;
    std::vector<std::shared_ptr<Connection>> allConnections_;

    sync::Mutex pendingMutex_;
    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest>> pendingRequests_;

    sync::Mutex queueMutex_;
    sync::Semaphore queueItems_;
    std::deque<IncomingMessage> dispatchQueue_;

    sync::Mutex threadsMutex_;
    std::vector<std::thread> threads_;
    std::vector<int> listenerDescriptors_;
};

}  // namespace dsm::network

#endif
