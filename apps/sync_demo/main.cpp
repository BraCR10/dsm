#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dsm/dsm.h"

namespace {

constexpr const char* kConfigPath = "config/cluster.json";
constexpr int kHostHoldSeconds = 3;
constexpr useconds_t kGuestHeadStartMicroseconds = 500000;

using Clock = std::chrono::steady_clock;

struct CounterWork {
    long long* counter = nullptr;
    int mutexId = -1;
    int barrierId = -1;
    int iterations = 0;
};

struct SharedCounter {
    long long* counter = nullptr;
    int mutexId = -1;
    int workBarrierId = -1;
    int doneBarrierId = -1;
};

void printUsage() {
    std::printf("Usage:\n"
                "  sync_demo [--node <id>] counter <threads> <iterations> [<nodes>]\n"
                "  sync_demo [--node <id>] join <address> <mutex_id> <work_barrier> <done_barrier> <threads> "
                "<iterations>\n"
                "  sync_demo [--node <id>] host\n"
                "  sync_demo [--node <id>] guest <mutex_id> <barrier_id>\n");
}

bool parseNumber(const char* text, long& outValue) {
    char* end = nullptr;
    outValue = std::strtol(text, &end, 0);
    return end != text && *end == '\0' && outValue >= 0;
}

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool succeeded(const char* operation, int status) {
    if (status < 0) {
        std::printf("%s failed with status %d\n", operation, status);
        return false;
    }
    return true;
}

void* incrementCounter(void* argument) {
    const CounterWork* work = static_cast<const CounterWork*>(argument);
    dsm_barrier(work->barrierId);
    for (int iteration = 0; iteration < work->iterations; ++iteration) {
        dsm_mutex_lock(work->mutexId);
        *work->counter += 1;
        dsm_mutex_unlock(work->mutexId);
    }
    dsm_barrier(work->barrierId);
    return nullptr;
}

long long runIncrements(const SharedCounter& shared, int threadCount, int iterations) {
    CounterWork work{shared.counter, shared.mutexId, shared.workBarrierId, iterations};
    std::vector<pthread_t> threads(static_cast<size_t>(threadCount));
    for (pthread_t& thread : threads) {
        pthread_create(&thread, nullptr, incrementCounter, &work);
    }
    for (pthread_t& thread : threads) {
        pthread_join(thread, nullptr);
    }
    return *shared.counter;
}

int runCounter(int threadCount, int iterations, int nodeCount) {
    SharedCounter shared;
    shared.counter = static_cast<long long*>(dsm_malloc(sizeof(long long)));
    shared.mutexId = dsm_mutex_init();
    shared.workBarrierId = dsm_barrier_init(threadCount * nodeCount);
    shared.doneBarrierId = dsm_barrier_init(nodeCount);
    if (shared.counter == nullptr || !succeeded("dsm_mutex_init", shared.mutexId) ||
        !succeeded("dsm_barrier_init", shared.workBarrierId) || !succeeded("dsm_barrier_init", shared.doneBarrierId)) {
        return 1;
    }
    *shared.counter = 0;
    std::printf("%d node(s) with %d threads each add %d to a shared counter at %p protected by mutex 0x%x\n",
                nodeCount, threadCount, iterations, static_cast<void*>(shared.counter), shared.mutexId);
    if (nodeCount > 1) {
        std::printf("Run on each other node: sync_demo join %p 0x%x 0x%x 0x%x %d %d\n",
                    static_cast<void*>(shared.counter), shared.mutexId, shared.workBarrierId, shared.doneBarrierId,
                    threadCount, iterations);
    }
    std::fflush(stdout);
    const Clock::time_point start = Clock::now();
    const long long result = runIncrements(shared, threadCount, iterations);
    const long long expected = static_cast<long long>(threadCount) * iterations * nodeCount;
    std::printf("Counter %lld, expected %lld, %s (%.2f s)\n", result, expected,
                result == expected ? "correct" : "WRONG", secondsSince(start));
    dsm_barrier(shared.doneBarrierId);
    dsm_mutex_destroy(shared.mutexId);
    dsm_barrier_destroy(shared.workBarrierId);
    dsm_barrier_destroy(shared.doneBarrierId);
    dsm_free(shared.counter);
    return result == expected ? 0 : 1;
}

int runJoin(const SharedCounter& shared, int threadCount, int iterations) {
    std::printf("Joining the shared counter at %p\n", static_cast<void*>(shared.counter));
    std::fflush(stdout);
    const long long result = runIncrements(shared, threadCount, iterations);
    std::printf("Counter seen by this node after every node finished: %lld\n", result);
    dsm_barrier(shared.doneBarrierId);
    return 0;
}

int runHost() {
    const int mutexId = dsm_mutex_init();
    const int barrierId = dsm_barrier_init(2);
    if (!succeeded("dsm_mutex_init", mutexId) || !succeeded("dsm_barrier_init", barrierId)) {
        return 1;
    }
    std::printf("Mutex 0x%x and barrier 0x%x created\n"
                "Run on another node: sync_demo guest 0x%x 0x%x\n"
                "Waiting at the barrier for the guest...\n",
                mutexId, barrierId, mutexId, barrierId);
    std::fflush(stdout);
    if (!succeeded("dsm_barrier", dsm_barrier(barrierId))) {
        return 1;
    }
    dsm_mutex_lock(mutexId);
    std::printf("Both arrived. Holding the mutex for %d s\n", kHostHoldSeconds);
    std::fflush(stdout);
    sleep(kHostHoldSeconds);
    dsm_mutex_unlock(mutexId);
    std::printf("Mutex released\n");
    dsm_barrier(barrierId);
    std::printf("Guest finished. Destroying the mutex and the barrier\n");
    succeeded("dsm_mutex_destroy", dsm_mutex_destroy(mutexId));
    succeeded("dsm_barrier_destroy", dsm_barrier_destroy(barrierId));
    return 0;
}

int runGuest(int mutexId, int barrierId) {
    std::printf("Arriving at barrier 0x%x\n", barrierId);
    std::fflush(stdout);
    if (!succeeded("dsm_barrier", dsm_barrier(barrierId))) {
        return 1;
    }
    usleep(kGuestHeadStartMicroseconds);
    std::printf("Requesting mutex 0x%x\n", mutexId);
    std::fflush(stdout);
    const Clock::time_point start = Clock::now();
    if (!succeeded("dsm_mutex_lock", dsm_mutex_lock(mutexId))) {
        return 1;
    }
    std::printf("Mutex acquired after waiting %.2f s\n", secondsSince(start));
    dsm_mutex_unlock(mutexId);
    dsm_barrier(barrierId);
    return 0;
}

int runCommand(int argumentCount, char** arguments, int first) {
    const char* command = arguments[first];
    long firstValue = 0;
    long secondValue = 0;
    long nodeCount = 1;
    if (std::strcmp(command, "counter") == 0 && (argumentCount == first + 3 || argumentCount == first + 4) &&
        parseNumber(arguments[first + 1], firstValue) && parseNumber(arguments[first + 2], secondValue) &&
        firstValue > 0 && (argumentCount == first + 3 || (parseNumber(arguments[first + 3], nodeCount) && nodeCount > 0))) {
        return runCounter(static_cast<int>(firstValue), static_cast<int>(secondValue), static_cast<int>(nodeCount));
    }
    if (std::strcmp(command, "join") == 0 && argumentCount == first + 7) {
        long values[6] = {};
        for (int index = 0; index < 6; ++index) {
            if (!parseNumber(arguments[first + 1 + index], values[index])) {
                printUsage();
                return 2;
            }
        }
        const SharedCounter shared{reinterpret_cast<long long*>(values[0]), static_cast<int>(values[1]),
                                   static_cast<int>(values[2]), static_cast<int>(values[3])};
        return runJoin(shared, static_cast<int>(values[4]), static_cast<int>(values[5]));
    }
    if (std::strcmp(command, "host") == 0 && argumentCount == first + 1) {
        return runHost();
    }
    if (std::strcmp(command, "guest") == 0 && argumentCount == first + 3 &&
        parseNumber(arguments[first + 1], firstValue) && parseNumber(arguments[first + 2], secondValue)) {
        return runGuest(static_cast<int>(firstValue), static_cast<int>(secondValue));
    }
    printUsage();
    return 2;
}

}  // namespace

int main(int argumentCount, char** arguments) {
    int nodeId = -1;
    int first = 1;
    long parsedNode = 0;
    if (argumentCount >= 3 && std::strcmp(arguments[1], "--node") == 0) {
        if (!parseNumber(arguments[2], parsedNode)) {
            printUsage();
            return 2;
        }
        nodeId = static_cast<int>(parsedNode);
        first = 3;
    }
    if (argumentCount <= first) {
        printUsage();
        return 2;
    }
    const int status = dsm_init(kConfigPath, nodeId, 0);
    if (status == DSM_ERR_NODE_UNAVAILABLE) {
        std::printf("No DSM node is running on this machine. Start one with dsm_node.\n");
        return 1;
    }
    if (status != DSM_OK) {
        std::printf("dsm_init failed with status %d\n", status);
        return 1;
    }
    const int result = runCommand(argumentCount, arguments, first);
    dsm_finalize();
    return result;
}
