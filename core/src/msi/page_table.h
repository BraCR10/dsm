#ifndef DSM_MSI_PAGE_TABLE_H
#define DSM_MSI_PAGE_TABLE_H

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "dsm/dsm.h"
#include "sync/condition_variable.h"
#include "sync/mutex.h"

namespace dsm::msi {

struct PageEntry {
    uint64_t pageId = 0;
    MsiState state = MSI_INVALID;
    uint32_t homeNode = 0;
    uint32_t storageNode = 0;
    uint32_t currentHolder = 0;
    std::set<uint32_t> copies;
    bool allocated = false;
};

class PageTable {
public:
    class Operation {
    public:
        Operation(PageTable& table, uint64_t pageId, uint32_t homeNode);
        ~Operation();
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;

        PageEntry read() const;
        void update(const std::function<void(PageEntry&)>& change);

    private:
        PageTable& table_;
        uint64_t pageId_;
    };

    std::vector<PageEntry> snapshot();

private:
    struct Slot {
        PageEntry entry;
        bool busy = false;
    };

    void acquire(uint64_t pageId, uint32_t homeNode);
    void release(uint64_t pageId);

    sync::Mutex mutex_;
    sync::ConditionVariable released_;
    std::map<uint64_t, Slot> slots_;
};

}  // namespace dsm::msi

#endif
