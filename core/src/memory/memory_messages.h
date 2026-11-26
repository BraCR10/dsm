#ifndef DSM_MEMORY_MEMORY_MESSAGES_H
#define DSM_MEMORY_MEMORY_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

namespace dsm::memory {

// MEMORY_REQUEST asks another node to store pages of an allocation whose home is the sender.
// The receiver answers MEMORY_GRANT with how many of those pages, from the first, it stores.
struct StorageRequestPayload {
    uint64_t firstPageId = 0;
    uint64_t pageCount = 0;
    uint64_t version = 0;
};

// STORAGE_RELEASE tells a node that the allocation it stored pages for was freed.
struct StorageReleasePayload {
    uint64_t firstPageId = 0;
    uint64_t pageCount = 0;
};

std::vector<uint8_t> encodeStorageRequest(const StorageRequestPayload& payload);
std::optional<StorageRequestPayload> decodeStorageRequest(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeStorageRelease(const StorageReleasePayload& payload);
std::optional<StorageReleasePayload> decodeStorageRelease(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeAddress(uint64_t address);
std::optional<uint64_t> decodeAddress(const std::vector<uint8_t>& bytes);

}  // namespace dsm::memory

#endif
