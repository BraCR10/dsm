#include "network/local_channel.h"

namespace dsm::network {

LocalChannel::LocalChannel(std::shared_ptr<Connection> connection, LocalRequestHandler handler,
                           RequestDispatch dispatch, LocalResponseObserver observer)
    : connection_(std::move(connection)),
      handler_(std::move(handler)),
      dispatch_(dispatch),
      observer_(std::move(observer)) {}

LocalChannel::~LocalChannel() {
    stop();
}

void LocalChannel::start(std::function<void()> onClosed) {
    onClosed_ = std::move(onClosed);
    running_.store(true);
    readerThread_ = std::thread(&LocalChannel::readLoop, this);
    workerThread_ = std::thread(&LocalChannel::workLoop, this);
}

void LocalChannel::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    connection_->shutdownSocket();
    queueItems_.post();
    if (readerThread_.joinable() && readerThread_.get_id() != std::this_thread::get_id()) {
        readerThread_.join();
    } else if (readerThread_.joinable()) {
        readerThread_.detach();
    }
    if (workerThread_.joinable() && workerThread_.get_id() != std::this_thread::get_id()) {
        workerThread_.join();
    } else if (workerThread_.joinable()) {
        workerThread_.detach();
    }
    failAllPending();
}

void LocalChannel::readLoop() {
    Message message;
    while (running_.load() && connection_->receive(message)) {
        if (isResponseType(message.type())) {
            if (observer_) {
                observer_(message);
            }
            std::shared_ptr<PendingRequest> pending;
            {
                sync::LockGuard guard(pendingMutex_);
                const auto found = pending_.find(message.header.requestId);
                if (found != pending_.end()) {
                    pending = found->second;
                    pending_.erase(found);
                }
            }
            if (pending) {
                pending->response = std::move(message);
                pending->completed.post();
            }
        } else if (dispatch_ == RequestDispatch::ReaderThread) {
            handler_(message);
        } else {
            {
                sync::LockGuard guard(queueMutex_);
                queue_.push_back(std::move(message));
            }
            queueItems_.post();
        }
        message = Message{};
    }
    failAllPending();
    if (onClosed_) {
        onClosed_();
    }
}

void LocalChannel::workLoop() {
    while (true) {
        queueItems_.wait();
        if (!running_.load()) {
            return;
        }
        Message request;
        {
            sync::LockGuard guard(queueMutex_);
            if (queue_.empty()) {
                continue;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        handler_(request);
    }
}

void LocalChannel::failAllPending() {
    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest>> failed;
    {
        sync::LockGuard guard(pendingMutex_);
        failed.swap(pending_);
    }
    for (auto& entry : failed) {
        entry.second->failed = true;
        entry.second->completed.post();
    }
}

DsmStatus LocalChannel::request(Message message, uint32_t timeoutMs, Message& outResponse) {
    const uint64_t requestId = nextRequestId_.fetch_add(1);
    auto pending = std::make_shared<PendingRequest>();
    {
        sync::LockGuard guard(pendingMutex_);
        pending_[requestId] = pending;
    }
    message.header.requestId = requestId;
    if (!connection_->send(message)) {
        sync::LockGuard guard(pendingMutex_);
        pending_.erase(requestId);
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    if (!pending->completed.waitFor(timeoutMs)) {
        sync::LockGuard guard(pendingMutex_);
        pending_.erase(requestId);
        return DSM_ERR_NETWORK;
    }
    if (pending->failed) {
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    outResponse = std::move(pending->response);
    return DSM_OK;
}

bool LocalChannel::reply(const Message& request, Message response) {
    response.header.requestId = request.header.requestId;
    response.header.sourceNode = request.header.destinationNode;
    response.header.destinationNode = request.header.sourceNode;
    return connection_->send(response);
}

}  // namespace dsm::network
