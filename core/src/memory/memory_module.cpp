#include "memory/memory_module.h"

#include <sys/mman.h>

#include <cstring>

#include "common/log.h"
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

DsmStatus MemoryModule::start() {
    const DsmStatus status = window_.createForNode(nodeSharedMemoryName(selfNodeId_), addressMap_.windowSize());
    if (status == DSM_OK) {
        common::logInfo(kModule, "shared window %s ready, local region at 0x%llx with %llu pages",
                        nodeSharedMemoryName(selfNodeId_).c_str(),
                        static_cast<unsigned long long>(addressMap_.regionStart(selfNodeId_)),
                        static_cast<unsigned long long>(registry_.totalPageCount()));
    }
    return status;
}

void MemoryModule::stop() {
    window_.close();
}

void MemoryModule::registerHandlers() {
    network_.registerHandler(MessageType::PageRequest, [this](const IncomingMessage& incoming) {
        handlePageRequest(incoming);
    });
    network_.registerHandler(MessageType::Invalidate, [this](const IncomingMessage& incoming) {
        handleInvalidate(incoming);
    });
}

uint8_t* MemoryModule::pageData(uint64_t pageId) const {
    return window_.data() + pageId * config_.pageSize;
}

uint64_t MemoryModule::localPageIndex(uint64_t pageId) const {
    return pageId - firstLocalPageId_;
}

void MemoryModule::restrictApplications(uint64_t pageId, int protection) {
    if (accessController_ != nullptr) {
        accessController_->restrictApplicationAccess(pageId, protection);
    }
}

uintptr_t MemoryModule::allocate(size_t size, uint32_t processId, DsmStatus& outStatus) {
    if (size == 0) {
        outStatus = DSM_ERR_INVALID_ARGUMENT;
        return 0;
    }
    const uint64_t pageCount = (size + config_.pageSize - 1) / config_.pageSize;
    const auto allocation = registry_.allocate(pageCount, processId);
    if (!allocation) {
        outStatus = DSM_ERR_NO_MEMORY;
        return 0;
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
    }
    outStatus = DSM_OK;
    return addressMap_.pageAddress(firstPageId);
}

DsmStatus MemoryModule::release(uintptr_t address, uint64_t& outPageCount) {
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
        restrictApplications(pageId, PROT_NONE);
        operation.update([](PageEntry& entry) {
            entry.allocated = false;
            entry.state = MSI_INVALID;
        });
        window_.discardPage(pageId * config_.pageSize, config_.pageSize);
    }
    outPageCount = allocation->pageCount;
    return DSM_OK;
}

void MemoryModule::resolveApplicationFault(uintptr_t address, bool isWrite, const FaultReply& replyWhilePageIsLocked) {
    counters_.pageFaults.fetch_add(1);
    const auto homeNode = addressMap_.homeNodeOf(address);
    if (!homeNode || config_.findNode(*homeNode) == nullptr) {
        replyWhilePageIsLocked({DSM_ERR_INVALID_POINTER, 0, PROT_NONE});
        return;
    }
    const uint64_t pageId = addressMap_.pageIdOf(address);
    const AccessType access = isWrite ? AccessType::Write : AccessType::Read;
    PageTable::Operation operation(msi_.pageTable(), pageId, *homeNode);
    const PageEntry entry = operation.read();

    FaultResolution resolution{DSM_OK, pageId, PROT_NONE};
    switch (msi_.onLocalFault(entry, selfNodeId_, access)) {
        case LocalFaultAction::GrantLocally:
            resolution.protection = protectionFor(entry.state);
            break;
        case LocalFaultAction::FetchReadFromHome:
            resolution.status = fetchSharedCopy(operation, pageId);
            resolution.protection = resolution.status == DSM_OK ? PROT_READ : PROT_NONE;
            break;
        case LocalFaultAction::InvalidateCopiesThenGrantWrite:
            invalidateCopies(operation, pageId);
            operation.update([](PageEntry& updated) { updated.state = MSI_MODIFIED; });
            resolution.protection = PROT_READ | PROT_WRITE;
            break;
        case LocalFaultAction::RejectNotAllocated:
            resolution.status = DSM_ERR_INVALID_POINTER;
            break;
        case LocalFaultAction::RejectRemoteWrite:
            resolution.status = DSM_ERR_NOT_OWNER;
            break;
    }
    replyWhilePageIsLocked(resolution);
}

DsmStatus MemoryModule::fetchSharedCopy(PageTable::Operation& operation, uint64_t pageId) {
    const uint32_t homeNode = operation.read().homeNode;
    Message response;
    const DsmStatus status = network_.request(
        homeNode, Channel::Main,
        makeMessage(MessageType::PageRequest, msi::encodePageRequest({pageId, AccessType::Read})),
        kPageRequestTimeoutMs, response);
    if (status != DSM_OK) {
        common::logError(kModule, "page %llu is unavailable: home node %u did not answer",
                         static_cast<unsigned long long>(pageId), homeNode);
        return DSM_ERR_PAGE_LOST;
    }
    if (response.header.status != DSM_OK) {
        return static_cast<DsmStatus>(response.header.status);
    }
    const auto pageDataPayload = msi::decodePageData(response.payload, config_.pageSize);
    if (!pageDataPayload || pageDataPayload->globalPageId != pageId) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    std::memcpy(pageData(pageId), pageDataPayload->content.data(), config_.pageSize);
    operation.update([homeNode](PageEntry& entry) {
        entry.state = MSI_SHARED;
        entry.currentHolder = homeNode;
    });
    counters_.pagesReceived.fetch_add(1);
    return DSM_OK;
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
                restrictApplications(request->globalPageId, PROT_READ);
            }
            operation.update([requester](PageEntry& updated) {
                updated.state = MSI_SHARED;
                updated.copies.insert(requester);
            });
            response.payload =
                msi::encodePageData(request->globalPageId, pageData(request->globalPageId), config_.pageSize);
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
    restrictApplications(*pageId, PROT_NONE);
    operation.update([](PageEntry& entry) { entry.state = MSI_INVALID; });
    response.payload = msi::encodePageId(*pageId);
    network_.reply(incoming, std::move(response));
}

DsmStatus MemoryModule::pageState(uintptr_t address, MsiState& outState) {
    if (!addressMap_.containsAddress(address)) {
        return DSM_ERR_INVALID_POINTER;
    }
    const uint64_t pageId = addressMap_.pageIdOf(address);
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
