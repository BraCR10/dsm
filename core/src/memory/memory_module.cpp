#include "memory/memory_module.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "common/log.h"
#include "memory/memory_messages.h"
#include "msi/page_messages.h"

namespace dsm::memory {

namespace {

constexpr const char* kModule = "memory";
constexpr uint32_t kPageRequestTimeoutMs = 10000;
constexpr uint32_t kInvalidateTimeoutMs = 2000;
constexpr uint32_t kRecallTimeoutMs = 2000;
constexpr int kRecallAttempts = 200;
constexpr useconds_t kRecallRetryDelayMicroseconds = 5000;
constexpr int kFetchAttempts = 1000;
constexpr useconds_t kFetchRetryDelayMicroseconds = 1000;
constexpr uint32_t kMemoryRequestTimeoutMs = 5000;

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
      registry_(sharedMemoryBytes / config.pageSize),
      waitingRequestWorker_([this](const IncomingMessage& incoming) { handleWaitingRequest(incoming); }) {}

DsmStatus MemoryModule::start() {
    const DsmStatus status = window_.createForNode(nodeSharedMemoryName(selfNodeId_), addressMap_.windowSize());
    if (status == DSM_OK) {
        common::logInfo(kModule, common::fromSelf(), "shared window %s ready, local region at 0x%llx with %llu pages",
                        nodeSharedMemoryName(selfNodeId_).c_str(),
                        static_cast<unsigned long long>(addressMap_.regionStart(selfNodeId_)),
                        static_cast<unsigned long long>(registry_.totalPageCount()));
        waitingRequestWorker_.start();
    }
    return status;
}

void MemoryModule::stop() {
    waitingRequestWorker_.stop();
    window_.close();
}

void MemoryModule::registerHandlers() {
    network_.registerHandler(MessageType::PageRequest, [this](const IncomingMessage& incoming) {
        waitingRequestWorker_.post(incoming);
    });
    network_.registerHandler(MessageType::MemoryFree, [this](const IncomingMessage& incoming) {
        waitingRequestWorker_.post(incoming);
    });
    network_.registerHandler(MessageType::MemoryRequest, [this](const IncomingMessage& incoming) {
        waitingRequestWorker_.post(incoming);
    });
    network_.registerHandler(MessageType::PageRecall, [this](const IncomingMessage& incoming) {
        handlePageRecall(incoming);
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
    const uintptr_t address = allocateLocally(pageCount, processId, outStatus);
    if (outStatus != DSM_ERR_NO_MEMORY) {
        return address;
    }
    return allocateOnAnotherNode(pageCount, processId, outStatus);
}

uintptr_t MemoryModule::allocateLocally(uint64_t pageCount, uint32_t processId, DsmStatus& outStatus) {
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

uintptr_t MemoryModule::allocateOnAnotherNode(uint64_t pageCount, uint32_t processId, DsmStatus& outStatus) {
    outStatus = DSM_ERR_NO_MEMORY;
    if (!remoteMemoryCandidates_) {
        return 0;
    }
    for (uint32_t candidate : remoteMemoryCandidates_(pageCount * config_.pageSize)) {
        Message response;
        const DsmStatus status = network_.request(
            candidate, Channel::Main,
            makeMessage(MessageType::MemoryRequest, encodeMemoryRequest({pageCount, processId})),
            kMemoryRequestTimeoutMs, response);
        const auto address = decodeAddress(response.payload);
        if (status == DSM_OK && response.header.status == DSM_OK && address) {
            common::logInfo(kModule, common::fromNode(candidate),
                            "granted %llu pages at 0x%llx because local memory is full",
                            static_cast<unsigned long long>(pageCount), static_cast<unsigned long long>(*address));
            outStatus = DSM_OK;
            return static_cast<uintptr_t>(*address);
        }
    }
    common::logError(kModule, common::fromSelf(),
                     "no node has %llu free pages", static_cast<unsigned long long>(pageCount));
    return 0;
}

DsmStatus MemoryModule::release(uintptr_t address, uint64_t& outPageCount) {
    const auto homeNode = addressMap_.homeNodeOf(address);
    if (!homeNode || config_.findNode(*homeNode) == nullptr || address % config_.pageSize != 0) {
        return DSM_ERR_INVALID_POINTER;
    }
    if (*homeNode != selfNodeId_) {
        return releaseOnHome(*homeNode, address, outPageCount);
    }
    return releaseLocally(address, outPageCount);
}

DsmStatus MemoryModule::releaseOnHome(uint32_t homeNode, uintptr_t address, uint64_t& outPageCount) {
    Message response;
    const DsmStatus status =
        network_.request(homeNode, Channel::Main, makeMessage(MessageType::MemoryFree, encodeAddress(address)),
                         kMemoryRequestTimeoutMs, response);
    if (status != DSM_OK) {
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    const auto pageCount = decodeAddress(response.payload);
    if (response.header.status == DSM_OK && pageCount) {
        outPageCount = *pageCount;
    }
    return static_cast<DsmStatus>(response.header.status);
}

DsmStatus MemoryModule::releaseLocally(uintptr_t address, uint64_t& outPageCount) {
    const uint64_t firstPageId = addressMap_.pageIdOf(address);
    const auto allocation = registry_.release(localPageIndex(firstPageId));
    if (!allocation) {
        return DSM_ERR_INVALID_POINTER;
    }
    for (uint64_t pageId = firstPageId; pageId < firstPageId + allocation->pageCount; ++pageId) {
        PageTable::Operation operation(msi_.pageTable(), pageId, selfNodeId_);
        const PageEntry entry = operation.read();
        std::set<uint32_t> remoteHolders = entry.copies;
        if (entry.currentHolder != selfNodeId_) {
            remoteHolders.insert(entry.currentHolder);
        }
        invalidateNodes(operation, pageId, remoteHolders);
        restrictApplications(pageId, PROT_NONE);
        operation.update([this](PageEntry& updated) {
            updated.allocated = false;
            updated.state = MSI_INVALID;
            updated.currentHolder = selfNodeId_;
            updated.copies.clear();
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
    if (*homeNode != selfNodeId_) {
        resolveRemoteFault(pageId, *homeNode, access, replyWhilePageIsLocked);
        return;
    }
    resolveHomeFault(pageId, access, replyWhilePageIsLocked);
}

void MemoryModule::resolveHomeFault(uint64_t pageId, AccessType access, const FaultReply& replyWhilePageIsLocked) {
    PageTable::Operation operation(msi_.pageTable(), pageId, selfNodeId_);
    const PageEntry entry = operation.read();
    FaultResolution resolution{DSM_OK, pageId, PROT_NONE};
    switch (msi_.onLocalFault(entry, selfNodeId_, access)) {
        case LocalFaultAction::GrantLocally:
            resolution.protection = protectionFor(entry.state);
            break;
        case LocalFaultAction::InvalidateCopiesThenGrantWrite:
            invalidateNodes(operation, pageId, entry.copies);
            operation.update([](PageEntry& updated) {
                updated.state = MSI_MODIFIED;
                updated.copies.clear();
            });
            resolution.protection = PROT_READ | PROT_WRITE;
            break;
        case LocalFaultAction::RecallFromHolderThenGrant: {
            resolution.status = recallFromHolder(operation, pageId, access);
            if (resolution.status != DSM_OK) {
                break;
            }
            const MsiState grantedState = access == AccessType::Write ? MSI_MODIFIED : MSI_SHARED;
            const uint64_t version = nextVersion(operation);
            operation.update([this, grantedState, version, previousHolder = entry.currentHolder](PageEntry& updated) {
                updated.state = grantedState;
                updated.version = version;
                updated.currentHolder = selfNodeId_;
                updated.copies.clear();
                if (grantedState == MSI_SHARED) {
                    updated.copies.insert(previousHolder);
                }
            });
            resolution.protection = protectionFor(grantedState);
            break;
        }
        case LocalFaultAction::RejectNotAllocated:
            resolution.status = DSM_ERR_INVALID_POINTER;
            break;
        case LocalFaultAction::FetchFromHome:
            resolution.status = DSM_ERR_INTERNAL;
            break;
    }
    replyWhilePageIsLocked(resolution);
}

void MemoryModule::resolveRemoteFault(uint64_t pageId, uint32_t homeNode, AccessType access,
                                      const FaultReply& replyWhilePageIsLocked) {
    for (int attempt = 0; attempt < kFetchAttempts; ++attempt) {
        {
            PageTable::Operation operation(msi_.pageTable(), pageId, homeNode);
            const PageEntry entry = operation.read();
            if (msi_.onLocalFault(entry, selfNodeId_, access) == LocalFaultAction::GrantLocally) {
                replyWhilePageIsLocked({DSM_OK, pageId, protectionFor(entry.state)});
                return;
            }
        }
        msi::PageDataPayload page;
        const DsmStatus status = requestPageFromHome(homeNode, pageId, access, page);
        if (status == DSM_ERR_BUSY) {
            usleep(kFetchRetryDelayMicroseconds);
            continue;
        }
        if (status != DSM_OK) {
            replyWhilePageIsLocked({status, pageId, PROT_NONE});
            return;
        }
        PageTable::Operation operation(msi_.pageTable(), pageId, homeNode);
        if (!msi::isNewerVersion(page.grant.version, operation.read().version)) {
            common::logInfo(kModule, common::fromSelf(),
                            "page %llu changed while it was requested, requesting it again",
                            static_cast<unsigned long long>(pageId));
            continue;
        }
        std::memcpy(pageData(pageId), page.content.data(), config_.pageSize);
        const MsiState grantedState = page.grant.grantedState;
        const uint32_t holder = grantedState == MSI_MODIFIED ? selfNodeId_ : homeNode;
        operation.update([grantedState, holder, version = page.grant.version](PageEntry& updated) {
            updated.state = grantedState;
            updated.currentHolder = holder;
            updated.version = version;
        });
        counters_.pagesReceived.fetch_add(1);
        replyWhilePageIsLocked({DSM_OK, pageId, protectionFor(grantedState)});
        return;
    }
    common::logError(kModule, common::fromSelf(),
                     "page %llu stayed busy after %d requests", static_cast<unsigned long long>(pageId),
                     kFetchAttempts);
    replyWhilePageIsLocked({DSM_ERR_BUSY, pageId, PROT_NONE});
}

DsmStatus MemoryModule::requestPageFromHome(uint32_t homeNode, uint64_t pageId, AccessType access,
                                            msi::PageDataPayload& outPage) {
    Message response;
    const DsmStatus status =
        network_.request(homeNode, Channel::Main,
                         makeMessage(MessageType::PageRequest, msi::encodePageRequest({pageId, access})),
                         kPageRequestTimeoutMs, response);
    if (status != DSM_OK) {
        common::logError(kModule, common::fromNode(homeNode),
                         "page %llu is unavailable because its home node did not answer",
                         static_cast<unsigned long long>(pageId));
        return DSM_ERR_PAGE_LOST;
    }
    if (response.header.status != DSM_OK) {
        return static_cast<DsmStatus>(response.header.status);
    }
    const auto page = msi::decodePageData(response.payload, config_.pageSize);
    if (!page || page->grant.globalPageId != pageId) {
        return DSM_ERR_INVALID_MESSAGE;
    }
    outPage = *page;
    return DSM_OK;
}

DsmStatus MemoryModule::recallFromHolder(PageTable::Operation& operation, uint64_t pageId, AccessType access) {
    const uint32_t holder = operation.read().currentHolder;
    for (int attempt = 0; attempt < kRecallAttempts; ++attempt) {
        Message response;
        const DsmStatus status =
            network_.request(holder, Channel::Main,
                             makeMessage(MessageType::PageRecall, msi::encodePageRequest({pageId, access})),
                             kRecallTimeoutMs, response);
        if (status != DSM_OK) {
            common::logError(kModule, common::fromNode(holder),
                             "page %llu is lost because its modified holder did not answer PAGE_RECALL",
                             static_cast<unsigned long long>(pageId));
            return DSM_ERR_PAGE_LOST;
        }
        if (response.header.status == DSM_ERR_BUSY) {
            usleep(kRecallRetryDelayMicroseconds);
            continue;
        }
        if (response.header.status != DSM_OK) {
            return static_cast<DsmStatus>(response.header.status);
        }
        const auto page = msi::decodePageData(response.payload, config_.pageSize);
        if (!page || page->grant.globalPageId != pageId) {
            return DSM_ERR_INVALID_MESSAGE;
        }
        std::memcpy(pageData(pageId), page->content.data(), config_.pageSize);
        counters_.pagesReceived.fetch_add(1);
        return DSM_OK;
    }
    common::logError(kModule, common::fromNode(holder), "page %llu stayed busy during PAGE_RECALL",
                     static_cast<unsigned long long>(pageId));
    return DSM_ERR_PAGE_LOST;
}

void MemoryModule::invalidateNodes(PageTable::Operation& operation, uint64_t pageId, const std::set<uint32_t>& nodes) {
    if (nodes.empty()) {
        return;
    }
    const uint64_t version = nextVersion(operation);
    for (uint32_t node : nodes) {
        Message response;
        const DsmStatus status = network_.request(
            node, Channel::Main, makeMessage(MessageType::Invalidate, msi::encodePageVersion({pageId, version})),
            kInvalidateTimeoutMs, response);
        if (status != DSM_OK) {
            common::logError(kModule, common::fromNode(node),
                             "INVALIDATE for page %llu was not confirmed, its copy is dropped",
                             static_cast<unsigned long long>(pageId));
        }
        counters_.invalidations.fetch_add(1);
    }
    operation.update([&nodes](PageEntry& updated) {
        for (uint32_t node : nodes) {
            updated.copies.erase(node);
        }
    });
}

uint64_t MemoryModule::nextVersion(PageTable::Operation& operation) {
    uint64_t version = 0;
    operation.update([&version](PageEntry& updated) { version = ++updated.version; });
    return version;
}

Message MemoryModule::pageDataMessage(uint64_t pageId, MsiState grantedState, uint64_t version) {
    counters_.pagesSent.fetch_add(1);
    return makeMessage(MessageType::PageData,
                       msi::encodePageData({pageId, grantedState, version}, pageData(pageId), config_.pageSize));
}

void MemoryModule::handleWaitingRequest(const IncomingMessage& incoming) {
    switch (incoming.message.type()) {
        case MessageType::MemoryFree:
            handleMemoryFree(incoming);
            return;
        case MessageType::MemoryRequest:
            handleMemoryRequest(incoming);
            return;
        default:
            handlePageRequest(incoming);
    }
}

void MemoryModule::handleMemoryRequest(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::MemoryGrant);
    const auto request = decodeMemoryRequest(incoming.message.payload);
    if (!request || request->pageCount == 0) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    DsmStatus status = DSM_OK;
    const uintptr_t address = allocateLocally(request->pageCount, request->processId, status);
    response.header.status = status;
    if (status == DSM_OK) {
        common::logInfo(kModule, common::fromNode(incoming.message.header.sourceNode),
                        "granted %llu pages at 0x%llx to remote process %u",
                        static_cast<unsigned long long>(request->pageCount), static_cast<unsigned long long>(address),
                        request->processId);
        response.payload = encodeAddress(address);
    }
    network_.reply(incoming, std::move(response));
}

void MemoryModule::handleMemoryFree(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::MemoryFreeAck);
    const auto address = decodeAddress(incoming.message.payload);
    uint64_t pageCount = 0;
    const auto homeNode = address ? addressMap_.homeNodeOf(*address) : std::nullopt;
    if (!address || !homeNode || *homeNode != selfNodeId_) {
        response.header.status = DSM_ERR_INVALID_POINTER;
    } else {
        response.header.status = releaseLocally(*address, pageCount);
    }
    if (response.header.status == DSM_OK) {
        response.payload = encodeAddress(pageCount);
    }
    network_.reply(incoming, std::move(response));
}

void MemoryModule::handlePageRequest(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::PageData);
    const auto request = msi::decodePageRequest(incoming.message.payload);
    if (!request) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    const uint64_t pageId = request->globalPageId;
    const auto homeNode = addressMap_.homeNodeOf(addressMap_.pageAddress(pageId));
    if (!homeNode || *homeNode != selfNodeId_) {
        response.header.status = DSM_ERR_NOT_FOUND;
        network_.reply(incoming, std::move(response));
        return;
    }
    const uint32_t requester = incoming.message.header.sourceNode;
    PageTable::Operation operation(msi_.pageTable(), pageId, selfNodeId_);
    const PageEntry entry = operation.read();
    const auto action = msi_.onRemoteRequest(entry, selfNodeId_, requester, request->access);
    if (action == RemoteRequestAction::RejectNotAllocated) {
        response.header.status = DSM_ERR_NOT_FOUND;
        network_.reply(incoming, std::move(response));
        return;
    }
    if (action == RemoteRequestAction::RequesterAlreadyHolds) {
        response.header.status = DSM_ERR_BUSY;
        network_.reply(incoming, std::move(response));
        return;
    }
    if (action == RemoteRequestAction::RecallThenShare || action == RemoteRequestAction::RecallThenGrantExclusive) {
        const DsmStatus status = recallFromHolder(operation, pageId, request->access);
        if (status != DSM_OK) {
            response.header.status = status;
            network_.reply(incoming, std::move(response));
            return;
        }
    }
    if (request->access == AccessType::Read) {
        if (entry.state == MSI_MODIFIED) {
            restrictApplications(pageId, PROT_READ);
        }
        const uint64_t version = nextVersion(operation);
        operation.update([this, requester, previousHolder = entry.currentHolder](PageEntry& updated) {
            if (previousHolder != selfNodeId_) {
                updated.copies.insert(previousHolder);
            }
            updated.state = MSI_SHARED;
            updated.currentHolder = selfNodeId_;
            updated.copies.insert(requester);
        });
        network_.reply(incoming, pageDataMessage(pageId, MSI_SHARED, version));
        return;
    }
    std::set<uint32_t> otherCopies = entry.copies;
    otherCopies.erase(requester);
    invalidateNodes(operation, pageId, otherCopies);
    restrictApplications(pageId, PROT_NONE);
    const uint64_t version = nextVersion(operation);
    operation.update([requester](PageEntry& updated) {
        updated.state = MSI_INVALID;
        updated.currentHolder = requester;
        updated.copies.clear();
    });
    network_.reply(incoming, pageDataMessage(pageId, MSI_MODIFIED, version));
}

void MemoryModule::handlePageRecall(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::PageData);
    const auto request = msi::decodePageRequest(incoming.message.payload);
    const auto homeNode =
        request ? addressMap_.homeNodeOf(addressMap_.pageAddress(request->globalPageId)) : std::nullopt;
    if (!request || !homeNode || *homeNode == selfNodeId_) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    const uint64_t pageId = request->globalPageId;
    PageTable::Operation operation(msi_.pageTable(), pageId, *homeNode);
    const PageEntry entry = operation.read();
    if (entry.state != MSI_MODIFIED) {
        response.header.status = DSM_ERR_BUSY;
        network_.reply(incoming, std::move(response));
        return;
    }
    const MsiState remainingState = msi::stateAfterRecall(request->access);
    restrictApplications(pageId, protectionFor(remainingState));
    response = pageDataMessage(pageId, remainingState, entry.version);
    operation.update([remainingState, home = *homeNode](PageEntry& updated) {
        updated.state = remainingState;
        updated.currentHolder = home;
    });
    network_.reply(incoming, std::move(response));
}

void MemoryModule::handleInvalidate(const IncomingMessage& incoming) {
    Message response = makeMessage(MessageType::InvalidateAck);
    const auto invalidation = msi::decodePageVersion(incoming.message.payload);
    const auto homeNode =
        invalidation ? addressMap_.homeNodeOf(addressMap_.pageAddress(invalidation->globalPageId)) : std::nullopt;
    if (!invalidation || !homeNode) {
        response.header.status = DSM_ERR_INVALID_MESSAGE;
        network_.reply(incoming, std::move(response));
        return;
    }
    const uint64_t pageId = invalidation->globalPageId;
    PageTable::Operation operation(msi_.pageTable(), pageId, *homeNode);
    if (msi::isNewerVersion(invalidation->version, operation.read().version)) {
        restrictApplications(pageId, PROT_NONE);
        operation.update([version = invalidation->version](PageEntry& updated) {
            updated.state = MSI_INVALID;
            updated.version = version;
        });
    }
    response.payload = msi::encodePageId(pageId);
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
