#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "dsm/dsm.h"

namespace {

constexpr const char* kConfigPath = "config/cluster.json";
constexpr size_t kMessageSize = 4096;

void printUsage() {
    std::printf("Usage:\n  demo_v1 [--node <id>] write <text>\n  demo_v1 [--node <id>] read <address>\n");
}

void waitForEnter(const char* prompt) {
    std::printf("%s\n", prompt);
    std::fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
}

int writeMessage(const char* text) {
    char* message = static_cast<char*>(dsm_malloc(kMessageSize));
    if (message == nullptr) {
        std::printf("dsm_malloc failed: %s\n", std::strerror(errno));
        return 1;
    }
    std::strncpy(message, text, kMessageSize - 1);
    std::printf("Message written at %p\n", static_cast<void*>(message));
    waitForEnter("Keep this program running while other nodes read the message.\n"
                 "Press Enter to free the message with dsm_free and finish.");
    dsm_free(message);
    return 0;
}

int readMessage(const char* addressText) {
    char* end = nullptr;
    const uintptr_t address = std::strtoull(addressText, &end, 0);
    if (end == addressText || *end != '\0') {
        printUsage();
        return 2;
    }
    const char* message = reinterpret_cast<const char*>(address);
    std::printf("Message read from %p: %s\n", static_cast<const void*>(message), message);
    MsiState state = MSI_INVALID;
    dsm_get_page_status(const_cast<char*>(message), &state);
    std::printf("Local page state: %s\n", state == MSI_SHARED ? "Shared" : state == MSI_MODIFIED ? "Modified" : "Invalid");
    waitForEnter("Press Enter to finish.");
    return 0;
}

}  // namespace

int main(int argumentCount, char** arguments) {
    int nodeId = -1;
    int first = 1;
    if (argumentCount >= 3 && std::strcmp(arguments[1], "--node") == 0) {
        char* end = nullptr;
        nodeId = static_cast<int>(std::strtol(arguments[2], &end, 10));
        if (end == arguments[2] || *end != '\0' || nodeId < 0) {
            printUsage();
            return 2;
        }
        first = 3;
    }
    if (argumentCount != first + 2 ||
        (std::strcmp(arguments[first], "write") != 0 && std::strcmp(arguments[first], "read") != 0)) {
        printUsage();
        return 2;
    }
    const int status = dsm_init(kConfigPath, nodeId, 0);
    if (status == DSM_ERR_NODE_UNAVAILABLE) {
        std::printf("No DSM node is running on this machine. Start one with dsm_node.\n");
        return 1;
    }
    if (status == DSM_ERR_INVALID_ARGUMENT) {
        std::printf("The requested node is not in the configuration or does not run on this machine.\n"
                    "Applications can only attach to a node of their own machine.\n");
        return 1;
    }
    if (status != DSM_OK) {
        std::printf("dsm_init failed with status %d\n", status);
        return 1;
    }
    const bool isWriter = std::strcmp(arguments[first], "write") == 0;
    const int result = isWriter ? writeMessage(arguments[first + 1]) : readMessage(arguments[first + 1]);
    dsm_finalize();
    return result;
}
