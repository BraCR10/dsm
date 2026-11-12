#include "msi/page_table.h"

namespace dsm::msi {

PageTable::Operation::Operation(PageTable& table, uint64_t pageId, uint32_t homeNode)
    : table_(table), pageId_(pageId) {
    table_.acquire(pageId, homeNode);
}

PageTable::Operation::~Operation() {
    table_.release(pageId_);
}

PageEntry PageTable::Operation::read() const {
    sync::LockGuard guard(table_.mutex_);
    return table_.slots_.at(pageId_).entry;
}

void PageTable::Operation::update(const std::function<void(PageEntry&)>& change) {
    sync::LockGuard guard(table_.mutex_);
    change(table_.slots_.at(pageId_).entry);
}

void PageTable::acquire(uint64_t pageId, uint32_t homeNode) {
    sync::LockGuard guard(mutex_);
    auto [slot, inserted] = slots_.try_emplace(pageId);
    if (inserted) {
        slot->second.entry.pageId = pageId;
        slot->second.entry.homeNode = homeNode;
        slot->second.entry.storageNode = homeNode;
        slot->second.entry.currentHolder = homeNode;
    }
    while (slot->second.busy) {
        released_.wait(mutex_);
    }
    slot->second.busy = true;
}

void PageTable::release(uint64_t pageId) {
    sync::LockGuard guard(mutex_);
    slots_.at(pageId).busy = false;
    released_.broadcast();
}

std::vector<PageEntry> PageTable::snapshot() {
    sync::LockGuard guard(mutex_);
    std::vector<PageEntry> entries;
    for (const auto& [pageId, slot] : slots_) {
        if (slot.entry.allocated || slot.entry.state != MSI_INVALID) {
            entries.push_back(slot.entry);
        }
    }
    return entries;
}

}  // namespace dsm::msi
