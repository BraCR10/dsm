#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "dsm/dsm.h"

namespace {

constexpr const char* kConfigPath = "config/cluster.json";
constexpr size_t kMessageSize = 4096;

void waitForEnter() {
    std::printf("Press Enter to finalize the node\n");
    std::string line;
    std::getline(std::cin, line);
}

int writeMessage(const char* text) {
    char* message = static_cast<char*>(dsm_malloc(kMessageSize));
    if (message == nullptr) {
        std::printf("dsm_malloc failed\n");
        return 1;
    }
    std::strncpy(message, text, kMessageSize - 1);
    std::printf("Message written at %p\n", static_cast<void*>(message));
    waitForEnter();
    return 0;
}

int readMessage(const char* addressText) {
    const char* message = reinterpret_cast<const char*>(std::strtoull(addressText, nullptr, 0));
    std::printf("Message read from %p: %s\n", static_cast<const void*>(message), message);
    MsiState state = MSI_INVALID;
    dsm_get_page_status(const_cast<char*>(message), &state);
    std::printf("Local page state: %s\n", state == MSI_SHARED ? "Shared" : state == MSI_MODIFIED ? "Modified" : "Invalid");
    waitForEnter();
    return 0;
}

}  // namespace

int main(int argumentCount, char** arguments) {
    if (argumentCount != 4 || (std::strcmp(arguments[2], "write") != 0 && std::strcmp(arguments[2], "read") != 0)) {
        std::printf("Usage:\n  demo <node> write <text>\n  demo <node> read <address>\n");
        return 2;
    }
    const int nodeId = std::atoi(arguments[1]);
    const int status = dsm_init(kConfigPath, nodeId, 0);
    if (status != DSM_OK) {
        std::printf("dsm_init failed with status %d\n", status);
        return 1;
    }
    const bool isWriter = std::strcmp(arguments[2], "write") == 0;
    const int result = isWriter ? writeMessage(arguments[3]) : readMessage(arguments[3]);
    dsm_finalize();
    return result;
}
