#include <cerrno>
#include <memory>

#include "api/node_runtime.h"
#include "common/log.h"
#include "dsm/dsm.h"
#include "settings/cluster_config.h"
#include "sync/mutex.h"

namespace {

dsm::sync::Mutex runtimeMutex;
std::unique_ptr<dsm::api::NodeRuntime> runtime;

int errnoFor(DsmStatus status) {
    switch (status) {
        case DSM_ERR_NO_MEMORY: return ENOMEM;
        case DSM_ERR_INVALID_ARGUMENT:
        case DSM_ERR_INVALID_POINTER: return EINVAL;
        case DSM_ERR_NOT_FOUND: return ENOENT;
        case DSM_ERR_SIZE_MISMATCH: return EEXIST;
        default: return EIO;
    }
}

dsm::api::NodeRuntime* activeRuntime() {
    dsm::sync::LockGuard guard(runtimeMutex);
    return runtime.get();
}

}  // namespace

extern "C" {

int dsm_init(const char* config_path, int node_id, size_t shared_memory_bytes) {
    if (config_path == nullptr || node_id < 0) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::sync::LockGuard guard(runtimeMutex);
    if (runtime) {
        return DSM_ERR_ALREADY_INITIALIZED;
    }
    dsm::settings::ClusterConfig config;
    const dsm::settings::ConfigResult result = dsm::settings::loadClusterConfig(config_path, config);
    if (result.status != DSM_OK) {
        dsm::common::logError("api", "%s", result.errorMessage.c_str());
        return result.status;
    }
    const dsm::settings::NodeConfig* self = config.findNode(static_cast<uint32_t>(node_id));
    if (self == nullptr) {
        dsm::common::logError("api", "node %d is not in the configuration", node_id);
        return DSM_ERR_INVALID_ARGUMENT;
    }
    const uint64_t sharedMemory = shared_memory_bytes == 0 ? self->memorySize : shared_memory_bytes;
    if (sharedMemory % config.pageSize != 0 || sharedMemory > config.nodeRangeSpacing) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    auto candidate = std::make_unique<dsm::api::NodeRuntime>(std::move(config), static_cast<uint32_t>(node_id),
                                                             sharedMemory);
    const DsmStatus status = candidate->start();
    if (status != DSM_OK) {
        return status;
    }
    runtime = std::move(candidate);
    return DSM_OK;
}

int dsm_finalize(void) {
    dsm::sync::LockGuard guard(runtimeMutex);
    if (!runtime) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    runtime->shutdown();
    runtime.reset();
    return DSM_OK;
}

void* dsm_malloc(size_t size) {
    dsm::api::NodeRuntime* node = activeRuntime();
    if (node == nullptr) {
        errno = EIO;
        return nullptr;
    }
    DsmStatus status = DSM_OK;
    void* pointer = node->memory().allocate(size, status);
    if (pointer == nullptr) {
        errno = errnoFor(status);
    }
    return pointer;
}

void dsm_free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    dsm::api::NodeRuntime* node = activeRuntime();
    if (node == nullptr || node->memory().release(ptr) != DSM_OK) {
        dsm::common::logFatal("api", "dsm_free received an invalid pointer %p", ptr);
    }
}

int dsm_get_stats(DsmStats* out_stats) {
    if (out_stats == nullptr) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::NodeRuntime* node = activeRuntime();
    if (node == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    *out_stats = node->stats().localStats();
    return DSM_OK;
}

int dsm_get_page_table(int node_id, PageTableEntry* out_entries, size_t max_entries) {
    if (node_id < 0 || (out_entries == nullptr && max_entries > 0)) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::NodeRuntime* node = activeRuntime();
    if (node == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    std::vector<PageTableEntry> entries;
    if (static_cast<uint32_t>(node_id) == node->selfNodeId()) {
        entries = node->memory().pageTableView();
    } else {
        const DsmStatus status = node->stats().remotePageTable(static_cast<uint32_t>(node_id), entries);
        if (status != DSM_OK) {
            return status;
        }
    }
    for (size_t index = 0; index < entries.size() && index < max_entries; ++index) {
        out_entries[index] = entries[index];
    }
    return static_cast<int>(entries.size());
}

int dsm_get_page_status(void* addr, MsiState* out_state) {
    if (addr == nullptr || out_state == nullptr) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::NodeRuntime* node = activeRuntime();
    if (node == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    return node->memory().pageState(addr, *out_state);
}

}  // extern "C"
