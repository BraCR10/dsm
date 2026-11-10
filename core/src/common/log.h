#ifndef DSM_COMMON_LOG_H
#define DSM_COMMON_LOG_H

#include <cstdint>

namespace dsm::common {

void setLogNodeId(uint32_t nodeId);
void logInfo(const char* module, const char* format, ...) __attribute__((format(printf, 2, 3)));
void logError(const char* module, const char* format, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void logFatal(const char* module, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace dsm::common

#endif
