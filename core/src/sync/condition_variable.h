#ifndef DSM_SYNC_CONDITION_VARIABLE_H
#define DSM_SYNC_CONDITION_VARIABLE_H

#include <pthread.h>

#include "sync/mutex.h"

namespace dsm::sync {

class ConditionVariable {
public:
    ConditionVariable() { pthread_cond_init(&handle_, nullptr); }
    ~ConditionVariable() { pthread_cond_destroy(&handle_); }
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void wait(Mutex& lockedMutex) { pthread_cond_wait(&handle_, lockedMutex.nativeHandle()); }
    void broadcast() { pthread_cond_broadcast(&handle_); }

private:
    pthread_cond_t handle_;
};

}  // namespace dsm::sync

#endif
