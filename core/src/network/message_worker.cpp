#include "network/message_worker.h"

namespace dsm::network {

MessageWorker::~MessageWorker() {
    stop();
}

void MessageWorker::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&MessageWorker::workLoop, this);
}

void MessageWorker::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    queueItems_.post();
    thread_.join();
}

void MessageWorker::post(const IncomingMessage& incoming) {
    {
        sync::LockGuard guard(queueMutex_);
        queue_.push_back(incoming);
    }
    queueItems_.post();
}

void MessageWorker::workLoop() {
    while (true) {
        queueItems_.wait();
        if (!running_.load()) {
            return;
        }
        IncomingMessage incoming;
        {
            sync::LockGuard guard(queueMutex_);
            if (queue_.empty()) {
                continue;
            }
            incoming = std::move(queue_.front());
            queue_.pop_front();
        }
        handler_(incoming);
    }
}

}  // namespace dsm::network
