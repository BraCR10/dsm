#include "api/client_runtime.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "api/local_messages.h"
#include "common/log.h"
#include "memory/fault_handler.h"
#include "network/unix_socket.h"
#include "stats/stats_messages.h"

namespace dsm::api {

namespace {

constexpr const char* kModule = "api";
constexpr uint32_t kLocalRequestTimeoutMs = 15000;

using network::makeMessage;
using network::Message;
using network::MessageType;

const char* faultFailureReason(int status) {
    switch (status) {
        case DSM_ERR_INVALID_POINTER: return "the address is not allocated";
        case DSM_ERR_NOT_OWNER: return "remote writes arrive in Prototype 2";
        case DSM_ERR_PAGE_LOST: return "the home node of the page is unavailable";
        case DSM_ERR_NODE_UNAVAILABLE: return "the local node stopped";
        default: return "the node could not resolve the fault";
    }
}

}  // namespace

ClientRuntime::ClientRuntime(settings::ClusterConfig config) : config_(std::move(config)), addressMap_(config_) {}

ClientRuntime::~ClientRuntime() {
    detach();
}

DsmStatus ClientRuntime::connectToNode(int requestedNodeId) {
    for (const settings::NodeConfig& node : config_.nodes) {
        const bool requested = requestedNodeId >= 0 && node.nodeId == static_cast<uint32_t>(requestedNodeId);
        const bool automatic = requestedNodeId < 0 && network::isLocalAddress(node.ip);
        if (!requested && !automatic) {
            continue;
        }
        auto connection = network::connectToUnixSocket(network::nodeSocketPath(node.nodeId));
        if (!connection) {
            continue;
        }
        channel_ = std::make_unique<network::LocalChannel>(
            connection, [this](const Message& request) { handleNodeRequest(request); },
            network::RequestDispatch::ReaderThread, [this](const Message& response) { observeResponse(response); });
        channel_->start();
        return DSM_OK;
    }
    return DSM_ERR_NODE_UNAVAILABLE;
}

DsmStatus ClientRuntime::attach(int requestedNodeId) {
    DsmStatus status = connectToNode(requestedNodeId);
    if (status != DSM_OK) {
        return status;
    }
    Message response;
    status = call(MessageType::LocalAttach,
                  encodeAttach({static_cast<uint32_t>(getpid()), config_.configHash}), response);
    const auto attachedNode = decodeValue(response.payload);
    if (status != DSM_OK || !attachedNode) {
        channel_->stop();
        return status == DSM_OK ? DSM_ERR_INVALID_MESSAGE : status;
    }
    nodeId_ = static_cast<uint32_t>(*attachedNode);
    common::setLogNodeId(nodeId_);
    status = window_.openForApplication(memory::nodeSharedMemoryName(nodeId_), addressMap_.windowSize(),
                                        addressMap_.windowStart());
    if (status != DSM_OK || !memory::installFaultHandler(addressMap_.windowStart(), addressMap_.windowSize(),
                                                         pendingFaults_)) {
        window_.close();
        channel_->stop();
        return status == DSM_OK ? DSM_ERR_INTERNAL : status;
    }
    resolverRunning_.store(true);
    resolverThread_ = std::thread(&ClientRuntime::resolverLoop, this);
    return DSM_OK;
}

void ClientRuntime::detach() {
    if (!resolverRunning_.exchange(false)) {
        return;
    }
    pendingFaults_.shutdown();
    resolverThread_.join();
    memory::uninstallFaultHandler();
    window_.close();
    channel_->stop();
}

DsmStatus ClientRuntime::call(MessageType type, std::vector<uint8_t> payload, Message& outResponse) {
    const DsmStatus status = channel_->request(makeMessage(type, std::move(payload)), kLocalRequestTimeoutMs,
                                               outResponse);
    if (status != DSM_OK) {
        return status;
    }
    return static_cast<DsmStatus>(outResponse.header.status);
}

void ClientRuntime::protectPage(uint64_t pageId, int protection) {
    if (mprotect(reinterpret_cast<void*>(addressMap_.pageAddress(pageId)), config_.pageSize, protection) != 0) {
        common::logFatal(kModule, "mprotect failed for page %llu: %s", static_cast<unsigned long long>(pageId),
                         std::strerror(errno));
    }
}

void ClientRuntime::handleNodeRequest(const Message& request) {
    if (request.type() != MessageType::LocalSetProtection) {
        return;
    }
    const auto protection = decodeProtection(request.payload);
    Message response = makeMessage(MessageType::LocalSetProtectionAck);
    if (protection) {
        protectPage(protection->pageId, static_cast<int>(protection->protection));
    } else {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
    }
    channel_->reply(request, std::move(response));
}

void ClientRuntime::observeResponse(const Message& response) {
    if (response.type() != MessageType::LocalFaultAck || response.header.status != DSM_OK) {
        return;
    }
    const auto grant = decodeProtection(response.payload);
    if (grant) {
        protectPage(grant->pageId, static_cast<int>(grant->protection));
    }
}

void ClientRuntime::resolverLoop() {
    memory::FaultRequest fault;
    size_t slotIndex = 0;
    while (pendingFaults_.takeNext(fault, slotIndex)) {
        resolveFault(fault);
        pendingFaults_.complete(slotIndex);
    }
}

void ClientRuntime::resolveFault(const memory::FaultRequest& fault) {
    Message response;
    const DsmStatus status = call(MessageType::LocalFault, encodeFault({fault.address, fault.isWrite}), response);
    if (status != DSM_OK) {
        common::logFatal(kModule, "%s access to %p failed: %s", fault.isWrite ? "write" : "read",
                         reinterpret_cast<void*>(fault.address), faultFailureReason(status));
    }
}

void* ClientRuntime::allocate(size_t size, DsmStatus& outStatus) {
    Message response;
    outStatus = call(MessageType::LocalMalloc, encodeValue(size), response);
    const auto address = decodeValue(response.payload);
    if (outStatus != DSM_OK || !address) {
        return nullptr;
    }
    return reinterpret_cast<void*>(*address);
}

DsmStatus ClientRuntime::release(void* pointer) {
    Message response;
    return call(MessageType::LocalFree, encodeValue(reinterpret_cast<uintptr_t>(pointer)), response);
}

DsmStatus ClientRuntime::stats(DsmStats& outStats) {
    Message response;
    const DsmStatus status = call(MessageType::StatsRequest, {}, response);
    const auto stats = stats::decodeStats(response.payload);
    if (status != DSM_OK) {
        return status;
    }
    if (!stats) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outStats = *stats;
    return DSM_OK;
}

DsmStatus ClientRuntime::pageTable(uint32_t nodeId, std::vector<PageTableEntry>& outEntries) {
    Message response;
    const DsmStatus status = call(MessageType::PageTableRequest, encodeValue(nodeId), response);
    const auto entries = stats::decodePageTable(response.payload);
    if (status != DSM_OK) {
        return status;
    }
    if (!entries) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outEntries = *entries;
    return DSM_OK;
}

DsmStatus ClientRuntime::pageStatus(const void* address, MsiState& outState) {
    Message response;
    const DsmStatus status =
        call(MessageType::LocalPageStatus, encodeValue(reinterpret_cast<uintptr_t>(address)), response);
    const auto state = decodeValue(response.payload);
    if (status != DSM_OK) {
        return status;
    }
    if (!state) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outState = static_cast<MsiState>(*state);
    return DSM_OK;
}

DsmStatus ClientRuntime::nodeTable(std::vector<NodeInfo>& outNodes) {
    Message response;
    const DsmStatus status = call(MessageType::LocalNodeTable, {}, response);
    const auto nodes = decodeNodeTable(response.payload);
    if (status != DSM_OK) {
        return status;
    }
    if (!nodes) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outNodes = *nodes;
    return DSM_OK;
}

}  // namespace dsm::api
