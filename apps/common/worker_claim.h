#ifndef DSM_APPS_COMMON_WORKER_CLAIM_H
#define DSM_APPS_COMMON_WORKER_CLAIM_H

#include <cstdint>
#include <cstdio>

#include "dsm/dsm.h"

namespace apps {

// The creator of a run is worker 0 and sets joinedWorkers to 1. Every process that joins takes the
// next free worker index under the run's DSM mutex, so worker indices never depend on DSM node ids.
// The creator destroys the mutex once every worker has joined, so a missing mutex means a full run.
inline bool claimWorkerIndex(int32_t mutexId, uint32_t& joinedWorkers, uint32_t workerCount,
                             uint32_t& outWorkerIndex) {
    const int lockStatus = joinedWorkers < workerCount ? dsm_mutex_lock(mutexId) : DSM_ERR_NOT_FOUND;
    if (lockStatus == DSM_ERR_NOT_FOUND) {
        std::fprintf(stderr, "this run already has its %u worker(s)\n", workerCount);
        return false;
    }
    if (lockStatus != DSM_OK) {
        std::fprintf(stderr, "dsm_mutex_lock failed with status %d\n", lockStatus);
        return false;
    }
    const bool hasFreeIndex = joinedWorkers < workerCount;
    if (hasFreeIndex) {
        outWorkerIndex = joinedWorkers;
        ++joinedWorkers;
    }
    const int unlockStatus = dsm_mutex_unlock(mutexId);
    if (unlockStatus != DSM_OK) {
        std::fprintf(stderr, "dsm_mutex_unlock failed with status %d\n", unlockStatus);
        return false;
    }
    if (!hasFreeIndex) {
        std::fprintf(stderr, "this run already has its %u worker(s)\n", workerCount);
    }
    return hasFreeIndex;
}

}

#endif
