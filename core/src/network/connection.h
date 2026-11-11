#ifndef DSM_NETWORK_CONNECTION_H
#define DSM_NETWORK_CONNECTION_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "network/message.h"
#include "sync/mutex.h"

namespace dsm::network {

class Connection {
public:
    Connection(int socketDescriptor, Channel channel);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    static std::shared_ptr<Connection> connectTo(const std::string& ip, uint16_t port, Channel channel,
                                                 uint32_t timeoutMs);

    bool send(const Message& message);
    bool receive(Message& outMessage);
    void shutdownSocket();

    Channel channel() const { return channel_; }
    uint32_t remoteNodeId() const { return remoteNodeId_.load(); }
    void setRemoteNodeId(uint32_t nodeId) { remoteNodeId_.store(nodeId); }
    bool isOpen() const { return open_.load(); }

private:
    int socketDescriptor_;
    Channel channel_;
    std::atomic<uint32_t> remoteNodeId_{kExternalClientNodeId};
    std::atomic<bool> open_{true};
    sync::Mutex sendMutex_;
};

}  // namespace dsm::network

#endif
