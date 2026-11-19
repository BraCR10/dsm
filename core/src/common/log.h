#ifndef DSM_COMMON_LOG_H
#define DSM_COMMON_LOG_H

#include <cstdint>
#include <string>

namespace dsm::common {

struct LogOrigin {
    enum class Kind { Self, Node, Process };
    Kind kind = Kind::Self;
    uint32_t id = 0;
};

inline LogOrigin fromSelf() {
    return LogOrigin{LogOrigin::Kind::Self, 0};
}

inline LogOrigin fromNode(uint32_t nodeId) {
    return LogOrigin{LogOrigin::Kind::Node, nodeId};
}

inline LogOrigin fromProcess(uint32_t processId) {
    return LogOrigin{LogOrigin::Kind::Process, processId};
}

std::string originText(const LogOrigin& origin);
std::string formatLogLine(const std::string& time, int64_t localNodeId, const char* level, const char* module,
                          const LogOrigin& origin, const char* message);

void setLogNodeId(uint32_t nodeId);
void logInfo(const char* module, const LogOrigin& origin, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void logError(const char* module, const LogOrigin& origin, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
[[noreturn]] void logFatal(const char* module, const LogOrigin& origin, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace dsm::common

#endif
