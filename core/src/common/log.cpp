#include "common/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace dsm::common {

namespace {

std::atomic<int64_t> currentNodeId{-1};

void writeLine(const char* level, const char* module, const char* format, va_list arguments) {
    char message[512];
    std::vsnprintf(message, sizeof(message), format, arguments);
    std::fprintf(stderr, "[node %lld] [%s] [%s] %s\n", static_cast<long long>(currentNodeId.load()), level, module,
                 message);
}

}  // namespace

void setLogNodeId(uint32_t nodeId) {
    currentNodeId.store(nodeId);
}

void logInfo(const char* module, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("INFO", module, format, arguments);
    va_end(arguments);
}

void logError(const char* module, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("ERROR", module, format, arguments);
    va_end(arguments);
}

void logFatal(const char* module, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("FATAL", module, format, arguments);
    va_end(arguments);
    std::_Exit(EXIT_FAILURE);
}

}  // namespace dsm::common
