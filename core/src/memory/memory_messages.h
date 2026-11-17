#ifndef DSM_MEMORY_MEMORY_MESSAGES_H
#define DSM_MEMORY_MEMORY_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

namespace dsm::memory {

struct MemoryRequestPayload {
    uint64_t pageCount = 0;
    uint32_t processId = 0;
};

std::vector<uint8_t> encodeMemoryRequest(const MemoryRequestPayload& payload);
std::optional<MemoryRequestPayload> decodeMemoryRequest(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeAddress(uint64_t address);
std::optional<uint64_t> decodeAddress(const std::vector<uint8_t>& bytes);

}  // namespace dsm::memory

#endif
