#ifndef DSM_MEMORY_ADDRESS_REGISTRY_H
#define DSM_MEMORY_ADDRESS_REGISTRY_H

#include <cstdint>
#include <optional>
#include <vector>

#include "sync/mutex.h"

namespace dsm::memory {

struct PageUsage {
    uint32_t processId = 0;
    uint32_t pageOrder = 0;
};

struct Allocation {
    uint64_t firstPageIndex = 0;
    uint64_t pageCount = 0;
};

class AddressRegistry {
public:
    explicit AddressRegistry(uint64_t pageCount);

    std::optional<Allocation> allocate(uint64_t pageCount, uint32_t processId);
    std::optional<Allocation> release(uint64_t firstPageIndex);
    bool isBlockStart(uint64_t pageIndex);
    std::optional<PageUsage> usageOf(uint64_t pageIndex);
    uint64_t freePageCount();
    uint64_t totalPageCount() const { return totalPageCount_; }

private:
    struct PageSlot {
        bool used = false;
        bool blockStart = false;
        uint64_t blockPageCount = 0;
        uint64_t blockStartIndex = 0;
        uint32_t processId = 0;
    };

    const uint64_t totalPageCount_;
    sync::Mutex mutex_;
    std::vector<PageSlot> slots_;
};

}  // namespace dsm::memory

#endif
