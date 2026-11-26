#include "compress_core.h"

#include <algorithm>
#include <cstring>

namespace compressor {

namespace {

constexpr size_t kWindowMask = kWindowSize - 1;
constexpr size_t kHashBits = 16;
constexpr size_t kHashSize = size_t{1} << kHashBits;
constexpr size_t kMaxChain = 128;
constexpr uint32_t kHashMultiplier = 2654435761u;
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint32_t hash3(const uint8_t* bytes) {
    const uint32_t value = (static_cast<uint32_t>(bytes[0]) << 16) |
                           (static_cast<uint32_t>(bytes[1]) << 8) | static_cast<uint32_t>(bytes[2]);
    return (value * kHashMultiplier) >> (32 - kHashBits);
}

void insertPosition(const uint8_t* input, size_t inputSize, size_t position, std::vector<int64_t>& head,
                    std::vector<int64_t>& previous) {
    if (position + kMinMatch > inputSize) {
        return;
    }
    const uint32_t hash = hash3(input + position);
    previous[position & kWindowMask] = head[hash];
    head[hash] = static_cast<int64_t>(position);
}

}

uint64_t worstCaseSize(uint64_t rawSize) {
    return rawSize + (rawSize + 7) / 8 + 1;
}

uint64_t checksum(const uint8_t* data, size_t size) {
    return checksumExtend(kFnvOffset, data, size);
}

uint64_t checksumExtend(uint64_t value, const uint8_t* data, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        value ^= data[index];
        value *= kFnvPrime;
    }
    return value;
}

bool encode(const uint8_t* input, size_t inputSize, std::vector<uint8_t>& output) {
    output.clear();
    if (inputSize == 0) {
        return true;
    }
    output.reserve(static_cast<size_t>(worstCaseSize(inputSize)));

    std::vector<int64_t> head(kHashSize, -1);
    std::vector<int64_t> previous(kWindowSize, -1);

    size_t position = 0;
    while (position < inputSize) {
        const size_t flagsIndex = output.size();
        output.push_back(0);
        uint8_t flags = 0;

        for (int token = 0; token < 8 && position < inputSize; ++token) {
            size_t bestLength = 0;
            size_t bestDistance = 0;
            const size_t maximumLength = std::min(kMaxMatch, inputSize - position);

            if (maximumLength >= kMinMatch) {
                const uint32_t hash = hash3(input + position);
                int64_t candidate = head[hash];
                size_t chain = 0;
                while (candidate >= 0 && chain < kMaxChain) {
                    const size_t candidatePosition = static_cast<size_t>(candidate);
                    if (candidatePosition < position && position - candidatePosition <= kWindowSize) {
                        size_t length = 0;
                        while (length < maximumLength &&
                               input[candidatePosition + length] == input[position + length]) {
                            ++length;
                        }
                        if (length > bestLength) {
                            bestLength = length;
                            bestDistance = position - candidatePosition;
                            if (length == maximumLength) {
                                break;
                            }
                        }
                    }
                    const int64_t next = previous[candidatePosition & kWindowMask];
                    if (next < 0 || next >= candidate) {
                        break;
                    }
                    candidate = next;
                    ++chain;
                }
            }

            if (bestLength >= kMinMatch) {
                const uint16_t packed = static_cast<uint16_t>(((bestLength - kMinMatch) << 12) |
                                                              (bestDistance - 1));
                output.push_back(static_cast<uint8_t>(packed & 0xFF));
                output.push_back(static_cast<uint8_t>(packed >> 8));
                for (size_t offset = 0; offset < bestLength; ++offset) {
                    insertPosition(input, inputSize, position + offset, head, previous);
                }
                position += bestLength;
            } else {
                flags |= static_cast<uint8_t>(1u << token);
                output.push_back(input[position]);
                insertPosition(input, inputSize, position, head, previous);
                ++position;
            }
        }

        output[flagsIndex] = flags;
    }

    return output.size() <= worstCaseSize(inputSize);
}

bool decode(const uint8_t* input, size_t inputSize, uint8_t* output, size_t outputCapacity,
            size_t& outProduced) {
    size_t inputOffset = 0;
    size_t outputOffset = 0;

    while (inputOffset < inputSize) {
        const uint8_t flags = input[inputOffset++];
        for (int bit = 0; bit < 8 && inputOffset < inputSize; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                if (outputOffset >= outputCapacity) {
                    return false;
                }
                output[outputOffset++] = input[inputOffset++];
                continue;
            }
            if (inputOffset + 2 > inputSize) {
                return false;
            }
            const uint16_t packed = static_cast<uint16_t>(input[inputOffset]) |
                                    (static_cast<uint16_t>(input[inputOffset + 1]) << 8);
            inputOffset += 2;
            const size_t length = static_cast<size_t>(packed >> 12) + kMinMatch;
            const size_t distance = static_cast<size_t>(packed & 0x0FFF) + 1;
            if (distance > outputOffset || outputOffset + length > outputCapacity) {
                return false;
            }
            for (size_t offset = 0; offset < length; ++offset) {
                output[outputOffset] = output[outputOffset - distance];
                ++outputOffset;
            }
        }
    }

    outProduced = outputOffset;
    return true;
}

}
