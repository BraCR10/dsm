#include "memory/memory_module.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "common/log.h"
#include "memory/fault_handler.h"
#include "msi/page_messages.h"

namespace dsm::memory {

namespace {

constexpr const char* kModule = "memory";
constexpr uint32_t kPageRequestTimeoutMs = 5000;
constexpr uint32_t kInvalidateTimeoutMs = 2000;

using msi::LocalFaultAction;
using msi::PageEntry;
using msi::PageTable;
using msi::RemoteRequestAction;
using network::AccessType;
using network::Channel;
using network::IncomingMessage;
using network::makeMessage;
using network::Message;
using network::MessageType;

int protectionFor(MsiState state) {
    switch (state) {
        case MSI_MODIFIED: return PROT_READ | PROT_WRITE;
        case MSI_SHARED: return PROT_READ;
        case MSI_INVALID: return PROT_NONE;
    }
    return PROT_NONE;
}

}  // namespace

MemoryModule::MemoryModule(const settings::ClusterConfig& config, uint32_t selfNodeId, uint64_t sharedMemoryBytes,
                           network::NetworkModule& network, msi::MsiModule& msi)
    : config_(config),
      selfNodeId_(selfNodeId),
      addressMap_(config),
      firstLocalPageId_(addressMap_.pageIdOf(addressMap_.regionStart(selfNodeId))),
      network_(network),
      msi_(msi),
      registry_(sharedMemoryBytes / config.pageSize) {}

MemoryModule::~MemoryModule() {
    stop();
}

DsmStatus MemoryModule::start() {
    void* requestedStart = reinterpret_cast<void*>(addressMap_.windowStart());
    window_ = mmap(requestedStart, addressMap_.windowSize(), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (window_ == MAP_FAILED || window_ != requestedStart) {
        common::logError(kModule, "cannot reserve the common window at %p: %s", requestedStart, std::strerror(errno));
        if (window_ != MAP_FAILED) {
            munmap(window_, addressMap_.windowSize());
        }
        window_ = nullptr;
        return DSM_ERR_NO_MEMORY;
    }
    if (!installFaultHandler(addressMap_.windowStart(), addressMap_.windowSize(), pendingFaults_)) {
        munmap(window_, addressMap_.windowSize());
        window_ = nullptr;
        return DSM_ERR_INTERNAL;
    }
    running_.store(true);
    resolverThread_ = std::thread(&MemoryModule::resolverLoop, this);
    common::logInfo(kModule, "window reserved at %p, local region at 0x%llx with %llu pages", window_,
                    static_cast<unsigned long long>(addressMap_.regionStart(selfNodeId_)),
                    static_cast<unsigned long long>(registry_.totalPageCount()));
    return DSM_OK;
}

void MemoryModule::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    pendingFaults_.shutdown();
    resolverThread_.join();
    uninstallFaultHandler();
    munmap(window_, addressMap_.windowSize());
    window_ = nullptr;
}

void MemoryModule::registerHandlers() {
    network_.registerHandler(MessageType::PageRequest, [this](const IncomingMessage& incoming) {
        handlePageRequest(incoming);
    });
    network_.registerHandler(MessageType::Invalidate, [this](const IncomingMessage& incoming) {
        handleInvalidate(incoming);
    });
}

void* MemoryModule::pagePointer(uint64_t pageId) const {
    return reinterpret_cast<void*>(addressMap_.pageAddress(pageId));
}

uint64_t MemoryModule::localPageIndex(uint64_t pageId) const {
    return pageId - firstLocalPageId_;
}

void MemoryModule::protectPage(uint64_t pageId, int protection) {
    if (mprotect(pagePointer(pageId), config_.pageSize, protection) != 0) {
        common::logFatal(kModule, "mprotect failed for page %llu: %s", static_cast<unsigned long long>(pageId),
                         std::strerror(errno));
    }
}

void* MemoryModule::allocate(size_t size, DsmStatus& outStatus) {
    if (size == 0) {
        outStatus = DSM_ERR_INVALID_ARGUMENT;
        return nullptr;
    }
    const uint64_t pageCount = (size + config_.pageSize - 1) / config_.pageSize;
    const auto allocation = registry_.allocate(pageCount, static_cast<uint32_t>(getpid()));
    if (!allocation) {
        outStatus = DSM_ERR_NO_MEMORY;
        return nullptr;
    }
    const uint64_t firstPageId = firstLocalPageId_ + allocation->firstPageIndex;
    for (uint64_t pageId = firstPageId; pageId < firstPageId + pageCount; ++pageId) {
        PageTable::Operation operation(msi_.pageTable(), pageId, selfNodeId_);
        operation.update([this](PageEntry& entry) {
            entry.allocated = true;
            entry.state = MSI_MODIFIED;
            entry.currentHolder = selfNodeId_;
            entry.copies.clear();
        });
        protectPage(pageId, PROT_READ | PROT_WRITE);
    }
    outStatus = DSM_OK;
    return pagePointer(firstPageId);
}

DsmStatus MemoryModule::release(void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto homeNode = addressMap_.homeNodeOf(address);
    if (!homeNode || *homeNode != selfNodeId_ || address % config_.pageSize != 0) {
        return DSM_ERR_INVALID_POINTER;
    }
    const uint64_t firstPageId = addressMap_.pageIdOf(address);
    const auto allocation = registry_.release(localPageIndex(firstPageId));
    if (!allocation) {
        return DSM_ERR_INVALID_POINTER;
    }
    for (uint64_t pageId = firstPageId; pageId < firstPageId + allocation->pageCount; ++pageId) {
        PageTable::Operation operation(msi_.pageTable(), pageId, selfNodeId_);
        invalidateCopies(operation, pageId);
        operation.update([](PageEntry& entry) {
            entry.allocated = false;
            entry.state = MSI_INVALID;
        });
        protectPage(pageId, PROT_NONE);
        madvise(pagePointer(pageId), config_.pageSize, MADV_DONTNEED);
    }
    return DSM_OK;
}

void MemoryModule::resolverLoop() {
    FaultRequest fault;
    size_t slotIndex = 0;
    while (pendingFaults_.takeNext(fault, slotIndex)) {
        resolveFault(fault);
        pendingFaults_.complete(slotIndex);
    }
}

void MemoryModule::resolveFault(const FaultRequest& fault) {
    counters_.pageFaults.fetch_add(1);
    const uint32_t homeNode = *addressMap_.homeNodeOf(fault.address);
    if (config_.findNode(homeNode) == nullptr) {
        common::logFatal(kModule, "address %p belongs to node %u, which is not in the configuration",
                         reinterpret_cast<void*>(fault.address), homeNode);
    }
    const uint64_t pageId = addressMap_.pageIdOf(fault.address);
    const AccessType access = fault.isWrite ? AccessType::Write : AccessType::Read;
    PageTable::Operation operation(msi_.pageTable(), pageId, homeNode);
    const PageEntry entry = operation.read();

    switch (msi_.onLocalFault(entry, selfNodeId_, access)) {
        case LocalFaultAction::GrantLocally:
            protectPage(pageId, protectionFor(entry.state));
            return;
        case LocalFaultAction::FetchReadFromHome:
            fetchSharedCopy(operation, pageId);
            return;
        case LocalFaultAction::InvalidateCopiesThenGrantWrite:
            invalidateCopies(operation, pageId);
            operation.update([](PageEntry& updated) { updated.state = MSI_MODIFIED; });
            protectPage(pageId, PROT_READ | PROT_WRITE);
            return;
        case LocalFaultAction::RejectNotAllocated:
            common::logFatal(kModule, "access to address %p, which is not allocated",
                             reinterpret_cast<void*>(fault.address));
        case LocalFaultAction::RejectRemoteWrite:
            common::logFatal(kModule, "write to remote address %p: remote writes arrive in Prototype 2",
                             reinterpret_cast<void*>(fault.address));
    }
}

void MemoryModule::fetchSharedCopy(PageTable::Operation& operation, uint64_t pageId) {
    const uint32_t homeNode = operation.read().homeNode;
    Message response;
    const DsmStatus status = network_.request(
        homeNode, Channel::Main,
        makeMessage(MessageType::PageRequest, msi::encodePageRequest({pageId, AccessType::Read})),
        kPageRequestTimeoutMs, response);
    if (status != DSM_OK) {
        common::logFatal(kModule, "page %llu is lost: home node %u is unavailable (status %d)",
                         static_cast<unsigned long long>(pageId), homeNode, status);
    }
    if (response.header.status != DSM_OK) {
        common::logFatal(kModule, "home node %u refused page %llu (status %d)", homeNode,
                         static_cast<unsigned long long>(pageId), response.header.status);
    }
    const auto pageData = msi::decodePageData(response.payload, config_.pageSize);
    if (!pageData || pageData->globalPageId != pageId) {
        common::logFatal(kModule, "home node %u sent an invalid PAGE_DATA for page %llu", homeNode,
                         static_cast<unsigned long long>(pageId));
    }
    protectPage(pageId, PROT_READ | PROT_WRITE);
    std::memcpy(pagePointer(pageId), pageData->content.data(), config_.pageSize);
    protectPage(pageId, PROT_READ);
    operation.update([homeNode](PageEntry& entry) {
        entry.state = MSI_SHARED;
        entry.currentHolder = homeNode;
    });
    counters_.pagesReceived.fetch_add(1);
}

void MemoryModule::invalidateCopies(PageTable::Operation& operation, uint64_t pageId) {
    const PageEntry entry = operation.read();
    for (uint32_t copyNode : entry.copies) {
        Message response;
        const DsmStatus status =
            network_.request(copyNode, Channel::Main, makeMessage(MessageType::Invalidate, msi::encodePageId(pageId)),
                             kInvalidateTimeoutMs, response);
        if (status != DSM_OK) {
            common::logError(kModule, "node %u did not confirm INVALIDATE for page %llu; its copy is dropped",
                             copyNode, static_cast<unsigned long long>(pageId));
        }
        counters_.invalidations.fetch_add(1);
    }
    operation.update([](PageEntry& updated) { updated.copies.clear(); });
}

void MemoryModule::handlePageRequest(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::PageData);
    const auto request = msi::decodePageRequest(incoming.message.payload);
    if (!request) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    const auto homeNode = addressMap_.homeNodeOf(addressMap_.pageAddress(request->globalPageId));
    if (!homeNode || *homeNode != selfNodeId_) {
        response.header.status = DSM_ERR_NOT_FOUND;
        network_.reply(incoming, std::move(response));
        return;
    }
    PageTable::Operation operation(msi_.pageTable(), request->globalPageId, selfNodeId_);
    const PageEntry entry = operation.read();
    switch (msi_.onRemoteRequest(entry, request->access)) {
        case RemoteRequestAction::RejectNotAllocated:
            response.header.status = DSM_ERR_NOT_FOUND;
            break;
        case RemoteRequestAction::RejectRemoteWrite:
            response.header.status = DSM_ERR_INVALID_MESSAGE;
            break;
        case RemoteRequestAction::ShareCopy: {
            const uint32_t requester = incoming.message.header.sourceNode;
            if (entry.state == MSI_MODIFIED) {
                protectPage(request->globalPageId, PROT_READ);
            }
            operation.update([requester](PageEntry& updated) {
                updated.state = MSI_SHARED;
                updated.copies.insert(requester);
            });
            response.payload = msi::encodePageData(request->globalPageId, pagePointer(request->globalPageId),
                                                   config_.pageSize);
            counters_.pagesSent.fetch_add(1);
            break;
        }
    }
    network_.reply(incoming, std::move(response));
}

void MemoryModule::handleInvalidate(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::InvalidateAck);
    const auto pageId = msi::decodePageId(incoming.message.payload);
    if (!pageId) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    const uint32_t homeNode = *addressMap_.homeNodeOf(addressMap_.pageAddress(*pageId));
    PageTable::Operation operation(msi_.pageTable(), *pageId, homeNode);
    protectPage(*pageId, PROT_NONE);
    operation.update([](PageEntry& entry) { entry.state = MSI_INVALID; });
    response.payload = msi::encodePageId(*pageId);
    network_.reply(incoming, std::move(response));
}

DsmStatus MemoryModule::pageState(const void* address, MsiState& outState) {
    const auto value = reinterpret_cast<uintptr_t>(address);
    if (!addressMap_.containsAddress(value)) {
        return DSM_ERR_INVALID_POINTER;
    }
    const uint64_t pageId = addressMap_.pageIdOf(value);
    outState = MSI_INVALID;
    for (const PageEntry& entry : msi_.pageTable().snapshot()) {
        if (entry.pageId == pageId) {
            outState = entry.state;
        }
    }
    return DSM_OK;
}

std::vector<PageTableEntry> MemoryModule::pageTableView() {
    std::vector<PageTableEntry> view;
    for (const PageEntry& entry : msi_.pageTable().snapshot()) {
        PageTableEntry row{};
        row.global_page_id = entry.pageId;
        row.home_node = entry.homeNode;
        row.storage_node = entry.storageNode;
        row.current_holder = entry.currentHolder;
        row.copy_count = static_cast<uint32_t>(entry.copies.size());
        row.state = static_cast<uint8_t>(entry.state);
        if (entry.homeNode == selfNodeId_) {
            if (const auto usage = registry_.usageOf(localPageIndex(entry.pageId))) {
                row.process_id = usage->processId;
                row.page_order = usage->pageOrder;
            }
        }
        view.push_back(row);
    }
    return view;
}

uint64_t MemoryModule::usedBytes() {
    return (registry_.totalPageCount() - registry_.freePageCount()) * config_.pageSize;
}

uint64_t MemoryModule::freeBytes() {
    return registry_.freePageCount() * config_.pageSize;
}

}  // namespace dsm::memory
