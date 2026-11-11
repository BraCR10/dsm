#include "network/message.h"
#include "test_runner.h"

using namespace dsm::network;

DSM_TEST(headerRoundTripPreservesEveryField) {
    MessageHeader header;
    header.type = static_cast<uint16_t>(MessageType::PageRequest);
    header.sourceNode = 3;
    header.destinationNode = 7;
    header.requestId = 0x0102030405060708ULL;
    header.payloadSize = 9;
    header.status = -15;

    uint8_t buffer[kHeaderSize];
    encodeHeader(header, buffer);
    MessageHeader decoded;
    EXPECT_TRUE(decodeHeader(buffer, decoded));
    EXPECT_EQ(kProtocolMagic, decoded.magic);
    EXPECT_EQ(header.type, decoded.type);
    EXPECT_EQ(3u, decoded.sourceNode);
    EXPECT_EQ(7u, decoded.destinationNode);
    EXPECT_EQ(header.requestId, decoded.requestId);
    EXPECT_EQ(9u, decoded.payloadSize);
    EXPECT_EQ(-15, decoded.status);
}

DSM_TEST(headerUsesNetworkByteOrderOnTheWire) {
    MessageHeader header;
    uint8_t buffer[kHeaderSize];
    encodeHeader(header, buffer);
    EXPECT_EQ('D', buffer[0]);
    EXPECT_EQ('S', buffer[1]);
    EXPECT_EQ('M', buffer[2]);
    EXPECT_EQ('1', buffer[3]);
}

DSM_TEST(decodeRejectsWrongMagic) {
    MessageHeader header;
    header.magic = 0x12345678;
    uint8_t buffer[kHeaderSize];
    encodeHeader(header, buffer);
    MessageHeader decoded;
    EXPECT_TRUE(!decodeHeader(buffer, decoded));
}

DSM_TEST(byteReaderReportsTruncatedPayload) {
    ByteWriter writer;
    writer.writeU32(42);
    std::vector<uint8_t> payload = writer.take();
    ByteReader reader(payload);
    EXPECT_EQ(42u, reader.readU32());
    reader.readU64();
    EXPECT_TRUE(!reader.ok());
}

DSM_TEST(fixedStringIsTruncatedAndNullTerminated) {
    ByteWriter writer;
    writer.writeFixedString("abcdef", 4);
    std::vector<uint8_t> payload = writer.take();
    EXPECT_EQ(4u, payload.size());
    ByteReader reader(payload);
    EXPECT_EQ(std::string("abc"), reader.readFixedString(4));
}
