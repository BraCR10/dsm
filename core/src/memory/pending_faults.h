#ifndef DSM_MEMORY_PENDING_FAULTS_H
#define DSM_MEMORY_PENDING_FAULTS_H

#include <semaphore.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dsm::memory {

struct FaultRequest {
    uintptr_t address = 0;
    bool isWrite = false;
};

// Registry of threads blocked on a page fault. The signal handler side only uses atomics,
// sem_post and sem_wait (a futex on Linux); all real work happens in the resolver thread.
class PendingFaults {
public:
    static constexpr size_t kSlotCount = 256;

    PendingFaults();
    ~PendingFaults();
    PendingFaults(const PendingFaults&) = delete;
    PendingFaults& operator=(const PendingFaults&) = delete;

    bool submitAndWait(const FaultRequest& request);
    bool takeNext(FaultRequest& outRequest, size_t& outSlotIndex);
    void complete(size_t slotIndex);
    void shutdown();
    size_t pendingCount() const;

private:
    enum SlotState : int { kFree, kFilling, kReady, kResolving };

    struct Slot {
        std::atomic<int> state{kFree};
        FaultRequest request;
        sem_t resolved;
    };

    std::array<Slot, kSlotCount> slots_;
    sem_t readyCount_;
    std::atomic<bool> running_{true};
};

}  // namespace dsm::memory

#endif
