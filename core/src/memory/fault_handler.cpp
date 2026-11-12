#include "memory/fault_handler.h"

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>

namespace dsm::memory {

namespace {

constexpr uint64_t kPageFaultWriteBit = 0x2;

std::atomic<uintptr_t> activeWindowStart{0};
std::atomic<uint64_t> activeWindowSize{0};
std::atomic<PendingFaults*> activePendingFaults{nullptr};
struct sigaction previousAction {};

bool faultWasWrite(void* context) {
#if defined(__x86_64__)
    const auto* userContext = static_cast<ucontext_t*>(context);
    return (userContext->uc_mcontext.gregs[REG_ERR] & kPageFaultWriteBit) != 0;
#else
    (void)context;
    return true;
#endif
}

void restoreDefaultAndReturn() {
    signal(SIGSEGV, SIG_DFL);
}

void handleSegmentationFault(int, siginfo_t* info, void* context) {
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t windowStart = activeWindowStart.load();
    PendingFaults* pendingFaults = activePendingFaults.load();
    if (pendingFaults == nullptr || address < windowStart || address - windowStart >= activeWindowSize.load()) {
        restoreDefaultAndReturn();
        return;
    }
    if (!pendingFaults->submitAndWait(FaultRequest{address, faultWasWrite(context)})) {
        static const char message[] = "dsm: too many concurrent page faults\n";
        if (write(STDERR_FILENO, message, sizeof(message) - 1) < 0) {
        }
        restoreDefaultAndReturn();
    }
}

}  // namespace

bool installFaultHandler(uintptr_t windowStart, uint64_t windowSize, PendingFaults& pendingFaults) {
    activeWindowStart.store(windowStart);
    activeWindowSize.store(windowSize);
    activePendingFaults.store(&pendingFaults);

    struct sigaction action {};
    action.sa_sigaction = handleSegmentationFault;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGSEGV, &action, &previousAction) == 0;
}

void uninstallFaultHandler() {
    sigaction(SIGSEGV, &previousAction, nullptr);
    activePendingFaults.store(nullptr);
    activeWindowSize.store(0);
}

}  // namespace dsm::memory
