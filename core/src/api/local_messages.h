#ifndef DSM_API_LOCAL_MESSAGES_H
#define DSM_API_LOCAL_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

#include "dsm/dsm.h"
#include "settings/cluster_config.h"

namespace dsm::api {

struct AttachPayload {
    uint32_t processId = 0;
    settings::ConfigHash configHash{};
};

struct FaultPayload {
    uint64_t address = 0;
    bool isWrite = false;
};

struct ProtectionPayload {
    uint64_t pageId = 0;
    uint32_t protection = 0;
};

std::vector<uint8_t> encodeAttach(const AttachPayload& payload);
std::optional<AttachPayload> decodeAttach(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeFault(const FaultPayload& payload);
std::optional<FaultPayload> decodeFault(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeProtection(const ProtectionPayload& payload);
std::optional<ProtectionPayload> decodeProtection(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeValue(uint64_t value);
std::optional<uint64_t> decodeValue(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodeNodeTable(const std::vector<NodeInfo>& nodes);
std::optional<std::vector<NodeInfo>> decodeNodeTable(const std::vector<uint8_t>& bytes);

}  // namespace dsm::api

#endif
