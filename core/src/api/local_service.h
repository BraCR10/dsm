#ifndef DSM_API_LOCAL_SERVICE_H
#define DSM_API_LOCAL_SERVICE_H

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <thread>

#include "dsm/dsm.h"
#include "memory/memory_module.h"
#include "memory/page_access_controller.h"
#include "network/local_channel.h"
#include "peers/peers_module.h"
#include "settings/cluster_config.h"
#include "stats/stats_module.h"
#include "sync/distributed_sync_module.h"
#include "sync/mutex.h"

namespace dsm::api {

// Node side of the DSM API: serves the applications and tools attached to this node.
class LocalService : public memory::PageAccessController {
public:
    LocalService(const settings::ClusterConfig& config, uint32_t selfNodeId, memory::MemoryModule& memory,
                 stats::StatsModule& stats, peers::PeersModule& peers, sync::DistributedSyncModule& sync);
    ~LocalService() override;
    LocalService(const LocalService&) = delete;
    LocalService& operator=(const LocalService&) = delete;

    DsmStatus start();
    void stop();
    void restrictApplicationAccess(uint64_t pageId, int protection) override;

private:
    struct Client {
        std::unique_ptr<network::LocalChannel> channel;
        std::atomic<bool> attached{false};
        std::atomic<bool> closed{false};
        uint32_t processId = 0;
        std::weak_ptr<Client> self;
    };

    void acceptLoop();
    void removeClosedClients();
    std::vector<std::shared_ptr<Client>> openClients();
    void handleRequest(Client& client, const network::Message& request);
    void handleAttach(Client& client, const network::Message& request);
    void handleFault(Client& client, const network::Message& request);
    void handleStats(Client& client, const network::Message& request);
    void handlePageTable(Client& client, const network::Message& request);
    void handleSyncCreate(Client& client, const network::Message& request);
    void handleSyncOperation(Client& client, const network::Message& request);
    sync::SyncCompletion replyWhenDone(Client& client, const network::Message& request, network::MessageType type);
    sync::SyncCompletion replyOrUnlockWhenGranted(Client& client, const network::Message& request,
                                                  uint32_t mutexId, const sync::SyncParticipant& requester);
    void onClientClosed(Client& client);
    void replyStatus(Client& client, const network::Message& request, network::MessageType type, DsmStatus status,
                     std::vector<uint8_t> payload = {});

    const settings::ClusterConfig& config_;
    const uint32_t selfNodeId_;
    memory::MemoryModule& memory_;
    stats::StatsModule& stats_;
    peers::PeersModule& peers_;
    sync::DistributedSyncModule& sync_;
    int listenerDescriptor_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    sync::Mutex clientsMutex_;
    std::list<std::shared_ptr<Client>> clients_;
};

}  // namespace dsm::api

#endif
