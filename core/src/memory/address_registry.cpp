#include "memory/address_registry.h"

namespace dsm::memory {

AddressRegistry::AddressRegistry(uint64_t pageCount) : totalPageCount_(pageCount), slots_(pageCount) {}

std::optional<Allocation> AddressRegistry::allocate(uint64_t pageCount, uint32_t processId) {
    if (pageCount == 0) {
        return std::nullopt;
    }
    sync::LockGuard guard(mutex_);
    uint64_t runStart = 0;
    uint64_t runLength = 0;
    for (uint64_t index = 0; index < totalPageCount_; ++index) {
        if (slots_[index].used) {
            runLength = 0;
            continue;
        }
        if (runLength == 0) {
            runStart = index;
        }
        if (++runLength == pageCount) {
            for (uint64_t offset = 0; offset < pageCount; ++offset) {
                PageSlot& slot = slots_[runStart + offset];
                slot.used = true;
                slot.blockStart = offset == 0;
                slot.blockPageCount = offset == 0 ? pageCount : 0;
                slot.processId = processId;
                slot.blockStartIndex = runStart;
            }
            return Allocation{runStart, pageCount};
        }
    }
    return std::nullopt;
}

std::optional<Allocation> AddressRegistry::release(uint64_t firstPageIndex) {
    sync::LockGuard guard(mutex_);
    if (firstPageIndex >= totalPageCount_ || !slots_[firstPageIndex].blockStart) {
        return std::nullopt;
    }
    const uint64_t pageCount = slots_[firstPageIndex].blockPageCount;
    for (uint64_t offset = 0; offset < pageCount; ++offset) {
        slots_[firstPageIndex + offset] = PageSlot{};
    }
    return Allocation{firstPageIndex, pageCount};
}

bool AddressRegistry::isBlockStart(uint64_t pageIndex) {
    sync::LockGuard guard(mutex_);
    return pageIndex < totalPageCount_ && slots_[pageIndex].blockStart;
}

std::optional<PageUsage> AddressRegistry::usageOf(uint64_t pageIndex) {
    sync::LockGuard guard(mutex_);
    if (pageIndex >= totalPageCount_ || !slots_[pageIndex].used) {
        return std::nullopt;
    }
    const PageSlot& slot = slots_[pageIndex];
    return PageUsage{slot.processId, static_cast<uint32_t>(pageIndex - slot.blockStartIndex)};
}

uint64_t AddressRegistry::freePageCount() {
    sync::LockGuard guard(mutex_);
    uint64_t freePages = 0;
    for (const PageSlot& slot : slots_) {
        freePages += slot.used ? 0 : 1;
    }
    return freePages;
}

}  // namespace dsm::memory
