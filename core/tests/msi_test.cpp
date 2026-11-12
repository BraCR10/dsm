#include "msi/msi_policy.h"
#include "msi/page_messages.h"
#include "test_runner.h"

using namespace dsm;
using msi::LocalFaultAction;
using msi::RemoteRequestAction;
using network::AccessType;

DSM_TEST(remoteReadOnInvalidPageFetchesFromHome) {
    EXPECT_TRUE(LocalFaultAction::FetchReadFromHome ==
                msi::decideLocalFault(false, false, MSI_INVALID, AccessType::Read));
}

DSM_TEST(remoteWriteIsRejectedInPrototypeOne) {
    EXPECT_TRUE(LocalFaultAction::RejectRemoteWrite ==
                msi::decideLocalFault(false, false, MSI_SHARED, AccessType::Write));
}

DSM_TEST(homeWriteOnSharedPageInvalidatesCopies) {
    EXPECT_TRUE(LocalFaultAction::InvalidateCopiesThenGrantWrite ==
                msi::decideLocalFault(true, true, MSI_SHARED, AccessType::Write));
}

DSM_TEST(homeAccessToUnallocatedPageIsRejected) {
    EXPECT_TRUE(LocalFaultAction::RejectNotAllocated ==
                msi::decideLocalFault(true, false, MSI_INVALID, AccessType::Read));
}

DSM_TEST(homeSharesAllocatedPagesAndRejectsUnallocatedOnes) {
    EXPECT_TRUE(RemoteRequestAction::ShareCopy == msi::decideRemoteRequest(true, AccessType::Read));
    EXPECT_TRUE(RemoteRequestAction::RejectNotAllocated == msi::decideRemoteRequest(false, AccessType::Read));
}

DSM_TEST(pageDataRoundTripKeepsTheWholePage) {
    std::vector<uint8_t> page(4096, 0xAB);
    page[4095] = 0x01;
    const auto decoded = msi::decodePageData(msi::encodePageData(42, page.data(), page.size()), page.size());
    EXPECT_TRUE(decoded.has_value());
    EXPECT_EQ(42u, decoded->globalPageId);
    EXPECT_TRUE(decoded->content == page);
}

DSM_TEST(pageRequestRejectsUnknownAccessType) {
    std::vector<uint8_t> bytes = msi::encodePageRequest({7, AccessType::Read});
    bytes.back() = 9;
    EXPECT_TRUE(!msi::decodePageRequest(bytes).has_value());
}
