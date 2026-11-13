#include <sys/socket.h>

#include "api/local_messages.h"
#include "network/local_channel.h"
#include "test_runner.h"

using namespace dsm;
using network::LocalChannel;
using network::makeMessage;
using network::Message;
using network::MessageType;

namespace {

std::pair<std::shared_ptr<network::Connection>, std::shared_ptr<network::Connection>> connectedPair() {
    int descriptors[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors);
    return {std::make_shared<network::Connection>(descriptors[0], network::Channel::Main),
            std::make_shared<network::Connection>(descriptors[1], network::Channel::Main)};
}

}  // namespace

DSM_TEST(localRequestIsAnsweredAndMatchedById) {
    auto [nodeSide, applicationSide] = connectedPair();
    LocalChannel* nodeChannelPointer = nullptr;
    LocalChannel nodeChannel(nodeSide, [&nodeChannelPointer](const Message& request) {
        const uint64_t size = *api::decodeValue(request.payload);
        nodeChannelPointer->reply(request, makeMessage(MessageType::LocalMallocAck, api::encodeValue(size * 2)));
    });
    nodeChannelPointer = &nodeChannel;
    LocalChannel applicationChannel(applicationSide, [](const Message&) {});
    nodeChannel.start();
    applicationChannel.start();

    Message response;
    EXPECT_EQ(DSM_OK, applicationChannel.request(makeMessage(MessageType::LocalMalloc, api::encodeValue(21)), 1000,
                                                 response));
    EXPECT_EQ(42u, *api::decodeValue(response.payload));
}

DSM_TEST(nodeCanRequestWhileApplicationWaitsForItsOwnResponse) {
    auto [nodeSide, applicationSide] = connectedPair();
    LocalChannel* nodeChannelPointer = nullptr;
    LocalChannel* applicationChannelPointer = nullptr;
    LocalChannel nodeChannel(nodeSide, [&nodeChannelPointer](const Message& request) {
        Message protectionAck;
        nodeChannelPointer->request(makeMessage(MessageType::LocalSetProtection, api::encodeProtection({7, 1})), 1000,
                                    protectionAck);
        nodeChannelPointer->reply(request, makeMessage(MessageType::LocalFaultAck,
                                                       api::encodeProtection({7, protectionAck.header.status == 0})));
    });
    nodeChannelPointer = &nodeChannel;
    uint64_t protectedPage = 0;
    LocalChannel applicationChannel(
        applicationSide,
        [&applicationChannelPointer, &protectedPage](const Message& request) {
            protectedPage = api::decodeProtection(request.payload)->pageId;
            applicationChannelPointer->reply(request, makeMessage(MessageType::LocalSetProtectionAck));
        },
        network::RequestDispatch::ReaderThread);
    applicationChannelPointer = &applicationChannel;
    nodeChannel.start();
    applicationChannel.start();

    Message response;
    EXPECT_EQ(DSM_OK, applicationChannel.request(makeMessage(MessageType::LocalFault, api::encodeFault({0, true})),
                                                 1000, response));
    EXPECT_EQ(7u, protectedPage);
    EXPECT_EQ(1u, api::decodeProtection(response.payload)->protection);
}

DSM_TEST(pendingRequestFailsWhenTheNodeCloses) {
    auto [nodeSide, applicationSide] = connectedPair();
    LocalChannel applicationChannel(applicationSide, [](const Message&) {});
    applicationChannel.start();
    nodeSide->shutdownSocket();
    Message response;
    EXPECT_EQ(DSM_ERR_NODE_UNAVAILABLE,
              applicationChannel.request(makeMessage(MessageType::LocalMalloc, api::encodeValue(1)), 1000, response));
}

DSM_TEST(nodeTableRoundTripKeepsEveryField) {
    NodeInfo node{};
    node.node_id = 3;
    std::snprintf(node.ip, sizeof(node.ip), "10.0.0.7");
    node.main_port = 8083;
    node.heartbeat_port = 8283;
    node.shared_memory_bytes = 1 << 20;
    node.free_memory_bytes = 4096;
    node.range_start = 0x103000000000ULL;
    node.is_active = 1;
    const auto decoded = api::decodeNodeTable(api::encodeNodeTable({node}));
    EXPECT_TRUE(decoded.has_value());
    EXPECT_EQ(1u, decoded->size());
    EXPECT_EQ(std::string("10.0.0.7"), std::string((*decoded)[0].ip));
    EXPECT_EQ(8283, (*decoded)[0].heartbeat_port);
    EXPECT_EQ(0x103000000000ULL, (*decoded)[0].range_start);
    EXPECT_EQ(1, (*decoded)[0].is_active);
}
