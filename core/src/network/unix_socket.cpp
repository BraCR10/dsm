#include "network/unix_socket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace dsm::network {

namespace {

constexpr int kListenBacklog = 16;

bool fillUnixAddress(const std::string& path, sockaddr_un& address) {
    if (path.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

std::string nodeSocketPath(uint32_t nodeId) {
    return "/tmp/dsm-node-" + std::to_string(nodeId) + ".sock";
}

int listenOnUnixSocket(const std::string& path) {
    sockaddr_un address{};
    if (!fillUnixAddress(path, address)) {
        return -1;
    }
    const int listenerDescriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenerDescriptor < 0) {
        return -1;
    }
    unlink(path.c_str());
    if (bind(listenerDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listenerDescriptor, kListenBacklog) != 0) {
        close(listenerDescriptor);
        return -1;
    }
    return listenerDescriptor;
}

std::shared_ptr<Connection> acceptUnixConnection(int listenerDescriptor) {
    const int socketDescriptor = accept4(listenerDescriptor, nullptr, nullptr, SOCK_CLOEXEC);
    if (socketDescriptor < 0) {
        return nullptr;
    }
    return std::make_shared<Connection>(socketDescriptor, Channel::Main);
}

std::shared_ptr<Connection> connectToUnixSocket(const std::string& path) {
    sockaddr_un address{};
    if (!fillUnixAddress(path, address)) {
        return nullptr;
    }
    const int socketDescriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketDescriptor < 0) {
        return nullptr;
    }
    if (connect(socketDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socketDescriptor);
        return nullptr;
    }
    return std::make_shared<Connection>(socketDescriptor, Channel::Main);
}

bool isLocalAddress(const std::string& ip) {
    in_addr target{};
    if (inet_pton(AF_INET, ip.c_str(), &target) != 1) {
        return false;
    }
    if ((ntohl(target.s_addr) >> 24) == 127) {
        return true;
    }
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return false;
    }
    bool found = false;
    for (ifaddrs* entry = interfaces; entry != nullptr && !found; entry = entry->ifa_next) {
        if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET) {
            found = reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr == target.s_addr;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

bool isTcpPortAvailable(uint16_t port) {
    const int probeDescriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probeDescriptor < 0) {
        return false;
    }
    const int enabled = 1;
    setsockopt(probeDescriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    const bool available = bind(probeDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close(probeDescriptor);
    return available;
}

}  // namespace dsm::network
