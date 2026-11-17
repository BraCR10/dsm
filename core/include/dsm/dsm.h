#ifndef DSM_DSM_H
#define DSM_DSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DsmStatus {
    DSM_OK = 0,
    DSM_ERR_INVALID_ARGUMENT = -1,
    DSM_ERR_NOT_INITIALIZED = -2,
    DSM_ERR_ALREADY_INITIALIZED = -3,
    DSM_ERR_CONFIG = -4,
    DSM_ERR_NETWORK = -5,
    DSM_ERR_NO_MEMORY = -6,
    DSM_ERR_NOT_FOUND = -7,
    DSM_ERR_ALREADY_EXISTS = -8,
    DSM_ERR_SIZE_MISMATCH = -9,
    DSM_ERR_BUSY = -10,
    DSM_ERR_NOT_OWNER = -11,
    DSM_ERR_DEADLOCK = -12,
    DSM_ERR_INVALID_POINTER = -13,
    DSM_ERR_NOT_REGISTERED = -14,
    DSM_ERR_NODE_UNAVAILABLE = -15,
    DSM_ERR_DUPLICATE_NODE = -16,
    DSM_ERR_CONFIG_MISMATCH = -17,
    DSM_ERR_PAGE_LOST = -18,
    DSM_ERR_INVALID_MESSAGE = -19,
    DSM_ERR_INTERNAL = -20
} DsmStatus;

typedef enum MsiState {
    MSI_INVALID = 0,
    MSI_SHARED = 1,
    MSI_MODIFIED = 2
} MsiState;

typedef struct PageTableEntry {
    uint64_t global_page_id;
    uint32_t home_node;
    uint32_t storage_node;
    uint32_t current_holder;
    uint32_t process_id;
    uint32_t page_order;
    uint32_t copy_count;
    uint8_t state;
} PageTableEntry;

typedef struct NodeInfo {
    uint32_t node_id;
    char ip[46];
    uint16_t main_port;
    uint16_t heartbeat_port;
    uint64_t shared_memory_bytes;
    uint64_t free_memory_bytes;
    uint64_t range_start;
    uint8_t is_active;
} NodeInfo;

typedef struct DsmStats {
    uint64_t page_faults;
    uint64_t pages_sent;
    uint64_t pages_received;
    uint64_t invalidations;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t used_memory_bytes;
    uint64_t free_memory_bytes;
} DsmStats;

int dsm_init(const char* config_path, int node_id, size_t shared_memory_bytes);
int dsm_finalize(void);

void* dsm_malloc(size_t size);
void dsm_free(void* ptr);

int dsm_mutex_init(void);
int dsm_mutex_lock(int mutex_id);
int dsm_mutex_unlock(int mutex_id);
int dsm_mutex_destroy(int mutex_id);

int dsm_barrier_init(int participant_count);
int dsm_barrier(int barrier_id);
int dsm_barrier_destroy(int barrier_id);

int dsm_get_stats(DsmStats* out_stats);
int dsm_get_node_stats(int node_id, DsmStats* out_stats);
int dsm_get_page_table(int node_id, PageTableEntry* out_entries, size_t max_entries);
int dsm_get_page_status(void* addr, MsiState* out_state);
int dsm_get_node_table(NodeInfo* out_nodes, size_t max_nodes);

#ifdef __cplusplus
}
#endif

#endif
