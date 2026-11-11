#include "network/network_module.h"
#include "test_runner.h"

using namespace dsm::network;

namespace {

constexpr uint16_t kFirstMainPort = 19180;
constexpr uint16_t kFirstHeartbeatPort = 19280;
constexpr uint16_t kSecondMainPort = 19181;
constexpr uint16_t kSecondHeartbeatPort = 19281;

}  // namespace

DSM_TEST(requestIsAnsweredOnTheSameConnection) {
    NetworkModule first(0);
    NetworkModule second(1);
    second.registerHandler(MessageType::StatsRequest, [&second](const IncomingMessage& incoming) {
        ByteWriter writer;
        writer.writeU64(incoming.message.header.sourceNode + 100);
        second.reply(incoming, makeMessage(MessageType::StatsData, writer.take()));
    });
    first.registerPeerAddress(1, "127.0.0.1", kSecondMainPort, kSecondHeartbeatPort);
    EXPECT_EQ(DSM_OK, first.start(kFirstMainPort, kFirstHeartbeatPort));
    EXPECT_EQ(DSM_OK, second.start(kSecondMainPort, kSecondHeartbeatPort));

    Message response;
    EXPECT_EQ(DSM_OK, first.request(1, Channel::Main, makeMessage(MessageType::StatsRequest), 2000, response));
    EXPECT_EQ(static_cast<uint16_t>(MessageType::StatsData), response.header.type);
    EXPECT_EQ(1u, response.header.sourceNode);
    ByteReader reader(response.payload);
    EXPECT_EQ(100u, reader.readU64());
    EXPECT_TRUE(first.messagesSent() >= 1);
    EXPECT_TRUE(second.messagesReceived() >= 1);
}

DSM_TEST(requestToUnknownNodeReportsUnavailable) {
    NetworkModule node(0);
    Message response;
    EXPECT_EQ(DSM_ERR_NODE_UNAVAILABLE,
              node.request(5, Channel::Main, makeMessage(MessageType::StatsRequest), 200, response));
}

DSM_TEST(requestToStoppedNodeReportsUnavailable) {
    NetworkModule node(0);
    node.registerPeerAddress(1, "127.0.0.1", 19189, 19289);
    Message response;
    EXPECT_EQ(DSM_ERR_NODE_UNAVAILABLE,
              node.request(1, Channel::Main, makeMessage(MessageType::StatsRequest), 200, response));
}

DSM_TEST(requestTimesOutWhenPeerNeverAnswers) {
    NetworkModule first(0);
    NetworkModule second(1);
    second.registerHandler(MessageType::StatsRequest, [](const IncomingMessage&) {});
    first.registerPeerAddress(1, "127.0.0.1", kSecondMainPort + 10, kSecondHeartbeatPort + 10);
    EXPECT_EQ(DSM_OK, second.start(kSecondMainPort + 10, kSecondHeartbeatPort + 10));
    Message response;
    EXPECT_EQ(DSM_ERR_NETWORK, first.request(1, Channel::Main, makeMessage(MessageType::StatsRequest), 200, response));
}
