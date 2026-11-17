#include <cerrno>
#include <memory>
#include <vector>

#include "api/client_runtime.h"
#include "common/log.h"
#include "dsm/dsm.h"
#include "settings/cluster_config.h"
#include "sync/mutex.h"

namespace {

dsm::sync::Mutex runtimeMutex;
std::unique_ptr<dsm::api::ClientRuntime> runtime;

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

dsm::api::ClientRuntime* activeRuntime() {
    dsm::sync::LockGuard guard(runtimeMutex);
    return runtime.get();
}

template <typename Entry>
int copyEntries(const std::vector<Entry>& entries, Entry* output, size_t capacity) {
    for (size_t index = 0; index < entries.size() && index < capacity; ++index) {
        output[index] = entries[index];
    }
    return static_cast<int>(entries.size());
}

using SyncOperation = DsmStatus (dsm::api::ClientRuntime::*)(uint32_t);

int runSyncOperation(int objectId, SyncOperation operation) {
    if (objectId < 0) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    return client == nullptr ? DSM_ERR_NOT_INITIALIZED : (client->*operation)(static_cast<uint32_t>(objectId));
}

}  // namespace

extern "C" {

int dsm_init(const char* config_path, int node_id, size_t shared_memory_bytes) {
    if (config_path == nullptr || shared_memory_bytes != 0) {
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
    auto candidate = std::make_unique<dsm::api::ClientRuntime>(std::move(config));
    const DsmStatus status = candidate->attach(node_id);
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
    runtime->detach();
    runtime.reset();
    return DSM_OK;
}

void* dsm_malloc(size_t size) {
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr) {
        errno = EIO;
        return nullptr;
    }
    DsmStatus status = DSM_OK;
    void* pointer = client->allocate(size, status);
    if (pointer == nullptr) {
        errno = errnoFor(status);
    }
    return pointer;
}

void dsm_free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr || client->release(ptr) != DSM_OK) {
        dsm::common::logFatal("api", "dsm_free received an invalid pointer %p", ptr);
    }
}

int dsm_mutex_init(void) {
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    int32_t mutexId = 0;
    const DsmStatus status = client->createMutex(mutexId);
    return status == DSM_OK ? mutexId : status;
}

int dsm_mutex_lock(int mutex_id) {
    return runSyncOperation(mutex_id, &dsm::api::ClientRuntime::lockMutex);
}

int dsm_mutex_unlock(int mutex_id) {
    return runSyncOperation(mutex_id, &dsm::api::ClientRuntime::unlockMutex);
}

int dsm_mutex_destroy(int mutex_id) {
    return runSyncOperation(mutex_id, &dsm::api::ClientRuntime::destroyMutex);
}

int dsm_barrier_init(int participant_count) {
    if (participant_count <= 0) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    int32_t barrierId = 0;
    const DsmStatus status = client->createBarrier(participant_count, barrierId);
    return status == DSM_OK ? barrierId : status;
}

int dsm_barrier(int barrier_id) {
    return runSyncOperation(barrier_id, &dsm::api::ClientRuntime::waitBarrier);
}

int dsm_barrier_destroy(int barrier_id) {
    return runSyncOperation(barrier_id, &dsm::api::ClientRuntime::destroyBarrier);
}

int dsm_get_stats(DsmStats* out_stats) {
    if (out_stats == nullptr) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    return client == nullptr ? DSM_ERR_NOT_INITIALIZED : client->stats(*out_stats);
}

int dsm_get_node_stats(int node_id, DsmStats* out_stats) {
    if (node_id < 0 || out_stats == nullptr) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    return client == nullptr ? DSM_ERR_NOT_INITIALIZED : client->nodeStats(static_cast<uint32_t>(node_id), *out_stats);
}

int dsm_get_page_table(int node_id, PageTableEntry* out_entries, size_t max_entries) {
    if (node_id < 0 || (out_entries == nullptr && max_entries > 0)) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    std::vector<PageTableEntry> entries;
    const DsmStatus status = client->pageTable(static_cast<uint32_t>(node_id), entries);
    return status == DSM_OK ? copyEntries(entries, out_entries, max_entries) : status;
}

int dsm_get_page_status(void* addr, MsiState* out_state) {
    if (addr == nullptr || out_state == nullptr) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    return client == nullptr ? DSM_ERR_NOT_INITIALIZED : client->pageStatus(addr, *out_state);
}

int dsm_get_node_table(NodeInfo* out_nodes, size_t max_nodes) {
    if (out_nodes == nullptr && max_nodes > 0) {
        return DSM_ERR_INVALID_ARGUMENT;
    }
    dsm::api::ClientRuntime* client = activeRuntime();
    if (client == nullptr) {
        return DSM_ERR_NOT_INITIALIZED;
    }
    std::vector<NodeInfo> nodes;
    const DsmStatus status = client->nodeTable(nodes);
    return status == DSM_OK ? copyEntries(nodes, out_nodes, max_nodes) : status;
}

}  // extern "C"
