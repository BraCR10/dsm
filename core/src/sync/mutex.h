#ifndef DSM_SYNC_MUTEX_H
#define DSM_SYNC_MUTEX_H

#include <pthread.h>

namespace dsm::sync {

class Mutex {
public:
    Mutex() { pthread_mutex_init(&handle_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&handle_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { pthread_mutex_lock(&handle_); }
    void unlock() { pthread_mutex_unlock(&handle_); }
    pthread_mutex_t* nativeHandle() { return &handle_; }

private:
    pthread_mutex_t handle_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~LockGuard() { mutex_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

}  // namespace dsm::sync

#endif
