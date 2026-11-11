#include "network/connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dsm::network {

namespace {

bool sendAll(int socketDescriptor, iovec* vectors, int vectorCount) {
    while (vectorCount > 0) {
        msghdr messageHeader{};
        messageHeader.msg_iov = vectors;
        messageHeader.msg_iovlen = static_cast<size_t>(vectorCount);
        const ssize_t written = sendmsg(socketDescriptor, &messageHeader, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        size_t remaining = static_cast<size_t>(written);
        while (vectorCount > 0 && remaining >= vectors->iov_len) {
            remaining -= vectors->iov_len;
            ++vectors;
            --vectorCount;
        }
        if (vectorCount > 0) {
            vectors->iov_base = static_cast<uint8_t*>(vectors->iov_base) + remaining;
            vectors->iov_len -= remaining;
        }
    }
    return true;
}

bool receiveAll(int socketDescriptor, void* buffer, size_t size) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    while (size > 0) {
        const ssize_t received = recv(socketDescriptor, cursor, size, 0);
        if (received == 0) {
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

void setTimeout(int socketDescriptor, int option, uint32_t timeoutMs) {
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = static_cast<suseconds_t>(timeoutMs % 1000) * 1000;
    setsockopt(socketDescriptor, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

}  // namespace

Connection::Connection(int socketDescriptor, Channel channel)
    : socketDescriptor_(socketDescriptor), channel_(channel) {
    const int enabled = 1;
    setsockopt(socketDescriptor_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

Connection::~Connection() {
    shutdownSocket();
    close(socketDescriptor_);
}

std::shared_ptr<Connection> Connection::connectTo(const std::string& ip, uint16_t port, Channel channel,
                                                  uint32_t timeoutMs) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        return nullptr;
    }
    const int socketDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketDescriptor < 0) {
        return nullptr;
    }
    setTimeout(socketDescriptor, SO_SNDTIMEO, timeoutMs);
    if (connect(socketDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socketDescriptor);
        return nullptr;
    }
    setTimeout(socketDescriptor, SO_SNDTIMEO, 0);
    return std::make_shared<Connection>(socketDescriptor, channel);
}

bool Connection::send(const Message& message) {
    MessageHeader header = message.header;
    header.payloadSize = static_cast<uint32_t>(message.payload.size());
    uint8_t headerBuffer[kHeaderSize];
    encodeHeader(header, headerBuffer);

    iovec vectors[2];
    vectors[0].iov_base = headerBuffer;
    vectors[0].iov_len = kHeaderSize;
    vectors[1].iov_base = const_cast<uint8_t*>(message.payload.data());
    vectors[1].iov_len = message.payload.size();

    sync::LockGuard guard(sendMutex_);
    if (!open_.load()) {
        return false;
    }
    const bool sent = sendAll(socketDescriptor_, vectors, message.payload.empty() ? 1 : 2);
    if (!sent) {
        open_.store(false);
    }
    return sent;
}

bool Connection::receive(Message& outMessage) {
    uint8_t headerBuffer[kHeaderSize];
    if (!receiveAll(socketDescriptor_, headerBuffer, kHeaderSize)) {
        open_.store(false);
        return false;
    }
    MessageHeader header;
    if (!decodeHeader(headerBuffer, header)) {
        open_.store(false);
        return false;
    }
    std::vector<uint8_t> payload(header.payloadSize);
    if (!payload.empty() && !receiveAll(socketDescriptor_, payload.data(), payload.size())) {
        open_.store(false);
        return false;
    }
    outMessage.header = header;
    outMessage.payload = std::move(payload);
    return true;
}

void Connection::shutdownSocket() {
    open_.store(false);
    shutdown(socketDescriptor_, SHUT_RDWR);
}

}  // namespace dsm::network
