#ifndef DSM_API_CLIENT_RUNTIME_H
#define DSM_API_CLIENT_RUNTIME_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "dsm/dsm.h"
#include "memory/address_map.h"
#include "memory/pending_faults.h"
#include "memory/shared_window.h"
#include "network/local_channel.h"
#include "settings/cluster_config.h"

namespace dsm::api {

// Application side of the DSM API: attaches to the local node, maps its shared window and
// turns page faults into requests to the node.
class ClientRuntime {
public:
    explicit ClientRuntime(settings::ClusterConfig config);
    ~ClientRuntime();
    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    DsmStatus attach(int requestedNodeId);
    void detach();

    void* allocate(size_t size, DsmStatus& outStatus);
    DsmStatus release(void* pointer);
    DsmStatus stats(DsmStats& outStats);
    DsmStatus pageTable(uint32_t nodeId, std::vector<PageTableEntry>& outEntries);
    DsmStatus pageStatus(const void* address, MsiState& outState);
    DsmStatus nodeTable(std::vector<NodeInfo>& outNodes);
    uint32_t nodeId() const { return nodeId_; }

private:
    DsmStatus connectToNode(int requestedNodeId);
    DsmStatus call(network::MessageType type, std::vector<uint8_t> payload, network::Message& outResponse);
    void handleNodeRequest(const network::Message& request);
    void observeResponse(const network::Message& response);
    void protectPage(uint64_t pageId, int protection);
    void resolverLoop();
    void resolveFault(const memory::FaultRequest& fault);

    const settings::ClusterConfig config_;
    const memory::AddressMap addressMap_;
    uint32_t nodeId_ = 0;
    std::unique_ptr<network::LocalChannel> channel_;
    memory::SharedWindow window_;
    memory::PendingFaults pendingFaults_;
    std::atomic<bool> resolverRunning_{false};
    std::thread resolverThread_;
};

}  // namespace dsm::api

#endif
