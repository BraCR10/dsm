#ifndef DSM_MEMORY_STORAGE_CAPACITY_H
#define DSM_MEMORY_STORAGE_CAPACITY_H

#include <cstdint>

#include "sync/mutex.h"

namespace dsm::memory {

// Physical pages a node stores: the pages of its own allocations it keeps locally plus the pages it
// stores for allocations whose home is another node. memory_size limits this count, not the addresses.
class StorageCapacity {
public:
    explicit StorageCapacity(uint64_t totalPages) : totalPages_(totalPages) {}

    uint64_t reserveUpTo(uint64_t pageCount);
    void release(uint64_t pageCount);
    uint64_t freePages();
    uint64_t usedPages();
    uint64_t totalPages() const { return totalPages_; }

private:
    const uint64_t totalPages_;
    sync::Mutex mutex_;
    uint64_t usedPages_ = 0;
};

}  // namespace dsm::memory

#endif
