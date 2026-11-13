#ifndef DSM_NETWORK_UNIX_SOCKET_H
#define DSM_NETWORK_UNIX_SOCKET_H

#include <cstdint>
#include <memory>
#include <string>

#include "network/connection.h"

namespace dsm::network {

std::string nodeSocketPath(uint32_t nodeId);

int listenOnUnixSocket(const std::string& path);
std::shared_ptr<Connection> acceptUnixConnection(int listenerDescriptor);
std::shared_ptr<Connection> connectToUnixSocket(const std::string& path);

bool isLocalAddress(const std::string& ip);
bool isTcpPortAvailable(uint16_t port);

}  // namespace dsm::network

#endif
