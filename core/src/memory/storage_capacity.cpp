#include "memory/storage_capacity.h"

#include <algorithm>

namespace dsm::memory {

uint64_t StorageCapacity::reserveUpTo(uint64_t pageCount) {
    sync::LockGuard guard(mutex_);
    const uint64_t granted = std::min(pageCount, totalPages_ - usedPages_);
    usedPages_ += granted;
    return granted;
}

void StorageCapacity::release(uint64_t pageCount) {
    sync::LockGuard guard(mutex_);
    usedPages_ -= std::min(pageCount, usedPages_);
}

uint64_t StorageCapacity::freePages() {
    sync::LockGuard guard(mutex_);
    return totalPages_ - usedPages_;
}

uint64_t StorageCapacity::usedPages() {
    sync::LockGuard guard(mutex_);
    return usedPages_;
}

}  // namespace dsm::memory
