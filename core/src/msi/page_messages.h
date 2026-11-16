#ifndef DSM_MSI_PAGE_MESSAGES_H
#define DSM_MSI_PAGE_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

#include "dsm/dsm.h"
#include "network/message.h"

namespace dsm::msi {

struct PageRequestPayload {
    uint64_t globalPageId = 0;
    network::AccessType access = network::AccessType::Read;
};

// Every change the home node makes to a page increases its version, so a node can tell whether a
// grant or an invalidation is newer than the copy it already has.
struct PageGrant {
    uint64_t globalPageId = 0;
    MsiState grantedState = MSI_INVALID;
    uint64_t version = 0;
};

struct PageDataPayload {
    PageGrant grant;
    std::vector<uint8_t> content;
};

struct PageVersionPayload {
    uint64_t globalPageId = 0;
    uint64_t version = 0;
};

inline bool isNewerVersion(uint64_t candidateVersion, uint64_t currentVersion) {
    return candidateVersion > currentVersion;
}

std::vector<uint8_t> encodePageRequest(const PageRequestPayload& payload);
std::optional<PageRequestPayload> decodePageRequest(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodePageData(const PageGrant& grant, const void* content, size_t contentSize);
std::optional<PageDataPayload> decodePageData(const std::vector<uint8_t>& bytes, size_t pageSize);

std::vector<uint8_t> encodePageVersion(const PageVersionPayload& payload);
std::optional<PageVersionPayload> decodePageVersion(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodePageId(uint64_t globalPageId);
std::optional<uint64_t> decodePageId(const std::vector<uint8_t>& bytes);

}  // namespace dsm::msi

#endif
