#include "memory/pending_faults.h"

#include <cerrno>

namespace dsm::memory {

namespace {

void waitIgnoringInterrupts(sem_t* semaphore) {
    while (sem_wait(semaphore) == -1 && errno == EINTR) {
    }
}

}  // namespace

PendingFaults::PendingFaults() {
    sem_init(&readyCount_, 0, 0);
    for (Slot& slot : slots_) {
        sem_init(&slot.resolved, 0, 0);
    }
}

PendingFaults::~PendingFaults() {
    for (Slot& slot : slots_) {
        sem_destroy(&slot.resolved);
    }
    sem_destroy(&readyCount_);
}

bool PendingFaults::submitAndWait(const FaultRequest& request) {
    for (Slot& slot : slots_) {
        int expected = kFree;
        if (!slot.state.compare_exchange_strong(expected, kFilling)) {
            continue;
        }
        slot.request = request;
        slot.state.store(kReady);
        sem_post(&readyCount_);
        waitIgnoringInterrupts(&slot.resolved);
        slot.state.store(kFree);
        return true;
    }
    return false;
}

bool PendingFaults::takeNext(FaultRequest& outRequest, size_t& outSlotIndex) {
    while (true) {
        waitIgnoringInterrupts(&readyCount_);
        if (!running_.load()) {
            return false;
        }
        for (size_t index = 0; index < kSlotCount; ++index) {
            int expected = kReady;
            if (slots_[index].state.compare_exchange_strong(expected, kResolving)) {
                outRequest = slots_[index].request;
                outSlotIndex = index;
                return true;
            }
        }
    }
}

void PendingFaults::complete(size_t slotIndex) {
    sem_post(&slots_[slotIndex].resolved);
}

void PendingFaults::shutdown() {
    running_.store(false);
    sem_post(&readyCount_);
}

size_t PendingFaults::pendingCount() const {
    size_t count = 0;
    for (const Slot& slot : slots_) {
        count += slot.state.load() == kFree ? 0 : 1;
    }
    return count;
}

}  // namespace dsm::memory
