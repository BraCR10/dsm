#include "memory/address_map.h"
#include "memory/address_registry.h"
#include "test_runner.h"

using namespace dsm;

namespace {

settings::ClusterConfig makeLayout() {
    settings::ClusterConfig config;
    config.baseAddress = 0x100000000000ULL;
    config.pageSize = 4096;
    config.addressWindowSize = 1ULL << 40;
    config.nodeRangeSpacing = 1ULL << 30;
    return config;
}

}  // namespace

DSM_TEST(homeNodeIsComputedFromTheAddress) {
    const memory::AddressMap map(makeLayout());
    EXPECT_EQ(0u, *map.homeNodeOf(0x100000000000ULL));
    EXPECT_EQ(2u, *map.homeNodeOf(0x100000000000ULL + 2 * (1ULL << 30) + 123));
    EXPECT_TRUE(!map.homeNodeOf(0x1000).has_value());
}

DSM_TEST(pageIdAndPageAddressAreInverse) {
    const memory::AddressMap map(makeLayout());
    const uintptr_t address = 0x100000000000ULL + 5 * 4096 + 17;
    const uint64_t pageId = map.pageIdOf(address);
    EXPECT_EQ(5u, pageId);
    EXPECT_EQ(0x100000000000ULL + 5 * 4096, map.pageAddress(pageId));
}

DSM_TEST(registryAllocatesContiguousPagesFirstFit) {
    memory::AddressRegistry registry(8);
    const auto first = registry.allocate(3, 10);
    const auto second = registry.allocate(2, 10);
    EXPECT_EQ(0u, first->firstPageIndex);
    EXPECT_EQ(3u, second->firstPageIndex);
    EXPECT_EQ(3u, registry.freePageCount());
    EXPECT_EQ(2u, registry.usageOf(2)->pageOrder);
    EXPECT_EQ(10u, registry.usageOf(4)->processId);
}

DSM_TEST(registryReusesReleasedBlocksAndRejectsInnerPointers) {
    memory::AddressRegistry registry(4);
    registry.allocate(2, 1);
    registry.allocate(2, 1);
    EXPECT_TRUE(!registry.allocate(1, 1).has_value());
    EXPECT_TRUE(!registry.release(1).has_value());
    EXPECT_EQ(2u, registry.release(0)->pageCount);
    EXPECT_EQ(0u, registry.allocate(1, 1)->firstPageIndex);
}
