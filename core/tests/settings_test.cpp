#include "settings/cluster_config.h"
#include "test_runner.h"

using dsm::settings::ClusterConfig;
using dsm::settings::ConfigResult;
using dsm::settings::parseClusterConfig;

namespace {

std::string makeConfig(const std::string& nodesJson, const std::string& pageSize = "4096") {
    return R"({"cluster":{"base_address":"0x100000000000","page_size":)" + pageSize +
           R"(,"address_window_size":70368744177664,"node_range_spacing":68719476736,)"
           R"("initial_memory_size":1048576},"heartbeat":{"interval_ms":500,"timeout_sec":2},"nodes":)" +
           nodesJson + "}";
}

const char* kTwoNodes =
    R"([{"node_id":0,"ip":"127.0.0.1","ports":{"main":8080,"heartbeat":8280}},)"
    R"({"node_id":1,"ip":"127.0.0.1","ports":{"main":8081,"heartbeat":8281},"memory_size":4194304}])";

}  // namespace

DSM_TEST(parsesValidConfigurationAndAppliesDefaultMemory) {
    ClusterConfig config;
    ConfigResult result = parseClusterConfig(makeConfig(kTwoNodes), config);
    EXPECT_EQ(DSM_OK, result.status);
    EXPECT_EQ(0x100000000000ULL, config.baseAddress);
    EXPECT_EQ(2u, config.nodes.size());
    EXPECT_EQ(1048576ULL, config.findNode(0)->memorySize);
    EXPECT_EQ(4194304ULL, config.findNode(1)->memorySize);
    EXPECT_EQ(8281, config.findNode(1)->heartbeatPort);
}

DSM_TEST(rejectsDuplicateNodeIds) {
    ClusterConfig config;
    const std::string nodes =
        R"([{"node_id":0,"ip":"127.0.0.1","ports":{"main":8080,"heartbeat":8280}},)"
        R"({"node_id":0,"ip":"127.0.0.1","ports":{"main":8081,"heartbeat":8281}}])";
    EXPECT_EQ(DSM_ERR_CONFIG, parseClusterConfig(makeConfig(nodes), config).status);
}

DSM_TEST(rejectsPageSizeThatIsNotPowerOfTwo) {
    ClusterConfig config;
    EXPECT_EQ(DSM_ERR_CONFIG, parseClusterConfig(makeConfig(kTwoNodes, "5000"), config).status);
}

DSM_TEST(rejectsMalformedJson) {
    ClusterConfig config;
    EXPECT_EQ(DSM_ERR_CONFIG, parseClusterConfig("{not json", config).status);
}

DSM_TEST(configHashIgnoresFormattingButNotContent) {
    ClusterConfig compact;
    ClusterConfig spaced;
    ClusterConfig different;
    std::string text = makeConfig(kTwoNodes);
    std::string spacedText = text;
    spacedText.insert(1, "\n   ");
    parseClusterConfig(text, compact);
    parseClusterConfig(spacedText, spaced);
    parseClusterConfig(makeConfig(kTwoNodes, "8192"), different);
    EXPECT_TRUE(compact.configHash == spaced.configHash);
    EXPECT_TRUE(compact.configHash != different.configHash);
}
