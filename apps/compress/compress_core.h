#ifndef DSM_APPS_COMPRESS_COMPRESS_CORE_H
#define DSM_APPS_COMPRESS_COMPRESS_CORE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace compressor {

constexpr uint32_t kCodecLzss = 1;
constexpr size_t kWindowSize = 4096;
constexpr size_t kMaxMatch = 18;
constexpr size_t kMinMatch = 3;

uint64_t worstCaseSize(uint64_t rawSize);
uint64_t checksum(const uint8_t* data, size_t size);
uint64_t checksumExtend(uint64_t value, const uint8_t* data, size_t size);

bool encode(const uint8_t* input, size_t inputSize, std::vector<uint8_t>& output);
bool decode(const uint8_t* input, size_t inputSize, uint8_t* output, size_t outputCapacity,
            size_t& outProduced);

}

#endif
