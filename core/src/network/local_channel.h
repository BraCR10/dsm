#ifndef DSM_NETWORK_LOCAL_CHANNEL_H
#define DSM_NETWORK_LOCAL_CHANNEL_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

#include "dsm/dsm.h"
#include "network/connection.h"
#include "network/message.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"

namespace dsm::network {

using LocalRequestHandler = std::function<void(const Message& request)>;
using LocalResponseObserver = std::function<void(const Message& response)>;

enum class RequestDispatch { WorkerThread, ReaderThread };

// Bidirectional request/response channel between an application and its local node.
// WorkerThread dispatch keeps the reader free while a handler waits for a response.
// ReaderThread dispatch processes requests and observed responses in arrival order.
class LocalChannel {
public:
    LocalChannel(std::shared_ptr<Connection> connection, LocalRequestHandler handler,
                 RequestDispatch dispatch = RequestDispatch::WorkerThread, LocalResponseObserver observer = {});
    ~LocalChannel();
    LocalChannel(const LocalChannel&) = delete;
    LocalChannel& operator=(const LocalChannel&) = delete;

    void start(std::function<void()> onClosed = {});
    void stop();

    DsmStatus request(Message message, uint32_t timeoutMs, Message& outResponse);
    bool reply(const Message& request, Message response);
    bool isOpen() const { return connection_->isOpen(); }

private:
    struct PendingRequest {
        sync::Semaphore completed;
        Message response;
        bool failed = false;
    };

    void readLoop();
    void workLoop();
    void failAllPending();

    std::shared_ptr<Connection> connection_;
    LocalRequestHandler handler_;
    RequestDispatch dispatch_;
    LocalResponseObserver observer_;
    std::function<void()> onClosed_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> nextRequestId_{1};

    sync::Mutex pendingMutex_;
    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest>> pending_;

    sync::Mutex queueMutex_;
    sync::Semaphore queueItems_;
    std::deque<Message> queue_;

    std::thread readerThread_;
    std::thread workerThread_;
};

}  // namespace dsm::network

#endif
