#include <chrono>
#include <thread>

#include "peers/peers_module.h"
#include "test_runner.h"

using namespace dsm;

namespace {

settings::ClusterConfig makeCluster(uint16_t basePort) {
    settings::ClusterConfig config;
    config.baseAddress = 0x100000000000ULL;
    config.pageSize = 4096;
    config.addressWindowSize = 1ULL << 40;
    config.nodeRangeSpacing = 1ULL << 30;
    config.initialMemorySize = 1 << 20;
    config.heartbeatIntervalMs = 50;
    config.heartbeatTimeoutSec = 1;
    for (uint32_t nodeId = 0; nodeId < 2; ++nodeId) {
        config.nodes.push_back({nodeId, "127.0.0.1", static_cast<uint16_t>(basePort + nodeId),
                                static_cast<uint16_t>(basePort + 100 + nodeId), 1 << 20});
    }
    return config;
}

struct TestNode {
    TestNode(const settings::ClusterConfig& config, uint32_t nodeId)
        : network(nodeId), peers(config, nodeId, 1 << 20, network, [] { return uint64_t{1 << 20}; }) {
        peers.registerHandlers();
        const settings::NodeConfig* self = config.findNode(nodeId);
        network.start(self->mainPort, self->heartbeatPort);
    }
    network::NetworkModule network;
    peers::PeersModule peers;
};

}  // namespace

DSM_TEST(joiningNodeAndExistingNodeSeeEachOtherActive) {
    const settings::ClusterConfig config = makeCluster(19400);
    TestNode first(config, 0);
    EXPECT_EQ(DSM_OK, first.peers.joinCluster());
    TestNode second(config, 1);
    EXPECT_EQ(DSM_OK, second.peers.joinCluster());
    EXPECT_TRUE(second.peers.isActive(0));
    EXPECT_TRUE(first.peers.isActive(1));
}

DSM_TEST(joinIsRejectedWhenConfigurationDiffers) {
    const settings::ClusterConfig config = makeCluster(19410);
    settings::ClusterConfig differentConfig = config;
    differentConfig.configHash[0] ^= 0xFF;
    TestNode first(config, 0);
    TestNode second(differentConfig, 1);
    EXPECT_EQ(DSM_ERR_CONFIG_MISMATCH, second.peers.joinCluster());
}

DSM_TEST(joinIsRejectedWhenNodeIdIsAlreadyActive) {
    const settings::ClusterConfig config = makeCluster(19420);
    TestNode first(config, 0);
    TestNode second(config, 1);
    EXPECT_EQ(DSM_OK, second.peers.joinCluster());

    settings::ClusterConfig impostorConfig = config;
    impostorConfig.nodes[1].mainPort = 19430;
    impostorConfig.nodes[1].heartbeatPort = 19530;
    impostorConfig.configHash = config.configHash;
    TestNode impostor(impostorConfig, 1);
    EXPECT_EQ(DSM_ERR_DUPLICATE_NODE, impostor.peers.joinCluster());
}

DSM_TEST(nodeIsDeclaredDownWhenHeartbeatsStop) {
    const settings::ClusterConfig config = makeCluster(19440);
    TestNode first(config, 0);
    first.peers.startHeartbeat();
    {
        TestNode second(config, 1);
        EXPECT_EQ(DSM_OK, second.peers.joinCluster());
        EXPECT_TRUE(first.peers.isActive(1));
        second.network.stop();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_TRUE(!first.peers.isActive(1));
}
