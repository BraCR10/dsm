#ifndef DSM_MEMORY_ADDRESS_MAP_H
#define DSM_MEMORY_ADDRESS_MAP_H

#include <cstdint>
#include <optional>

#include "settings/cluster_config.h"

namespace dsm::memory {

class AddressMap {
public:
    explicit AddressMap(const settings::ClusterConfig& config);

    bool containsAddress(uintptr_t address) const;
    uint64_t regionStart(uint32_t nodeId) const;
    std::optional<uint32_t> homeNodeOf(uintptr_t address) const;
    uint64_t pageIdOf(uintptr_t address) const;
    uintptr_t pageAddress(uint64_t pageId) const;
    uint64_t pageSize() const { return pageSize_; }
    uint64_t windowStart() const { return baseAddress_; }
    uint64_t windowSize() const { return windowSize_; }

private:
    uint64_t baseAddress_;
    uint64_t windowSize_;
    uint64_t pageSize_;
    uint64_t nodeRangeSpacing_;
};

}  // namespace dsm::memory

#endif
