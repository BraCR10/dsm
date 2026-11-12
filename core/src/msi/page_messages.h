#ifndef DSM_MSI_PAGE_MESSAGES_H
#define DSM_MSI_PAGE_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

#include "network/message.h"

namespace dsm::msi {

struct PageRequestPayload {
    uint64_t globalPageId = 0;
    network::AccessType access = network::AccessType::Read;
};

struct PageDataPayload {
    uint64_t globalPageId = 0;
    std::vector<uint8_t> content;
};

std::vector<uint8_t> encodePageRequest(const PageRequestPayload& payload);
std::optional<PageRequestPayload> decodePageRequest(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodePageData(uint64_t globalPageId, const void* content, size_t contentSize);
std::optional<PageDataPayload> decodePageData(const std::vector<uint8_t>& bytes, size_t pageSize);

std::vector<uint8_t> encodePageId(uint64_t globalPageId);
std::optional<uint64_t> decodePageId(const std::vector<uint8_t>& bytes);

}  // namespace dsm::msi

#endif
