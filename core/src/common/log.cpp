#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace dsm::common {

namespace {

constexpr int kNodeColumnWidth = 7;
constexpr int kLevelColumnWidth = 5;
constexpr int kModuleColumnWidth = 8;
constexpr int kOriginColumnWidth = 14;

std::atomic<int64_t> currentNodeId{-1};

std::string currentTime() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm localTime{};
    localtime_r(&seconds, &localTime);
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03lld", localTime.tm_hour, localTime.tm_min,
                  localTime.tm_sec, static_cast<long long>(milliseconds));
    return text;
}

std::string nodeText(int64_t localNodeId) {
    return localNodeId < 0 ? "node ?" : "node " + std::to_string(localNodeId);
}

void writeLine(const char* level, const char* module, const LogOrigin& origin, const char* format,
               va_list arguments) {
    char message[512];
    std::vsnprintf(message, sizeof(message), format, arguments);
    const std::string line = formatLogLine(currentTime(), currentNodeId.load(), level, module, origin, message);
    std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace

std::string originText(const LogOrigin& origin) {
    switch (origin.kind) {
        case LogOrigin::Kind::Node: return "node " + std::to_string(origin.id);
        case LogOrigin::Kind::Process: return "process " + std::to_string(origin.id);
        case LogOrigin::Kind::Self: break;
    }
    return "self";
}

std::string formatLogLine(const std::string& time, int64_t localNodeId, const char* level, const char* module,
                          const LogOrigin& origin, const char* message) {
    char line[768];
    std::snprintf(line, sizeof(line), "%s  %-*s  %-*s  %-*s  %-*s  %s", time.c_str(), kNodeColumnWidth,
                  nodeText(localNodeId).c_str(), kLevelColumnWidth, level, kModuleColumnWidth, module,
                  kOriginColumnWidth, originText(origin).c_str(), message);
    return line;
}

void setLogNodeId(uint32_t nodeId) {
    currentNodeId.store(nodeId);
}

void logInfo(const char* module, const LogOrigin& origin, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("INFO", module, origin, format, arguments);
    va_end(arguments);
}

void logError(const char* module, const LogOrigin& origin, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("ERROR", module, origin, format, arguments);
    va_end(arguments);
}

void logFatal(const char* module, const LogOrigin& origin, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    writeLine("FATAL", module, origin, format, arguments);
    va_end(arguments);
    std::_Exit(EXIT_FAILURE);
}

}  // namespace dsm::common
