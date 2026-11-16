#ifndef DSM_NETWORK_MESSAGE_WORKER_H
#define DSM_NETWORK_MESSAGE_WORKER_H

#include <atomic>
#include <deque>
#include <thread>

#include "network/network_module.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"

namespace dsm::network {

// Handles messages on a thread of its own, in arrival order. Handlers that wait for other nodes
// run here so the dispatcher thread stays free to answer the messages they wait for.
class MessageWorker {
public:
    explicit MessageWorker(MessageHandler handler) : handler_(std::move(handler)) {}
    ~MessageWorker();
    MessageWorker(const MessageWorker&) = delete;
    MessageWorker& operator=(const MessageWorker&) = delete;

    void start();
    void stop();
    void post(const IncomingMessage& incoming);

private:
    void workLoop();

    MessageHandler handler_;
    std::atomic<bool> running_{false};
    sync::Mutex queueMutex_;
    sync::Semaphore queueItems_;
    std::deque<IncomingMessage> queue_;
    std::thread thread_;
};

}  // namespace dsm::network

#endif
