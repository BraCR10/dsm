#ifndef DSM_SYNC_SEMAPHORE_H
#define DSM_SYNC_SEMAPHORE_H

#include <semaphore.h>

#include <cerrno>
#include <cstdint>
#include <ctime>

namespace dsm::sync {

class Semaphore {
public:
    explicit Semaphore(unsigned int initialValue = 0) { sem_init(&handle_, 0, initialValue); }
    ~Semaphore() { sem_destroy(&handle_); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void post() { sem_post(&handle_); }

    void wait() {
        while (sem_wait(&handle_) == -1 && errno == EINTR) {
        }
    }

    bool waitFor(uint32_t timeoutMs) {
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeoutMs / 1000;
        deadline.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        while (true) {
            if (sem_timedwait(&handle_, &deadline) == 0) {
                return true;
            }
            if (errno != EINTR) {
                return false;
            }
        }
    }

private:
    sem_t handle_;
};

}  // namespace dsm::sync

#endif
