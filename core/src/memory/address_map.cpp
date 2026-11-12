#include "memory/address_map.h"

namespace dsm::memory {

AddressMap::AddressMap(const settings::ClusterConfig& config)
    : baseAddress_(config.baseAddress),
      windowSize_(config.addressWindowSize),
      pageSize_(config.pageSize),
      nodeRangeSpacing_(config.nodeRangeSpacing) {}

bool AddressMap::containsAddress(uintptr_t address) const {
    return address >= baseAddress_ && address - baseAddress_ < windowSize_;
}

uint64_t AddressMap::regionStart(uint32_t nodeId) const {
    return baseAddress_ + static_cast<uint64_t>(nodeId) * nodeRangeSpacing_;
}

std::optional<uint32_t> AddressMap::homeNodeOf(uintptr_t address) const {
    if (!containsAddress(address)) {
        return std::nullopt;
    }
    return static_cast<uint32_t>((address - baseAddress_) / nodeRangeSpacing_);
}

uint64_t AddressMap::pageIdOf(uintptr_t address) const {
    return (address - baseAddress_) / pageSize_;
}

uintptr_t AddressMap::pageAddress(uint64_t pageId) const {
    return static_cast<uintptr_t>(baseAddress_ + pageId * pageSize_);
}

}  // namespace dsm::memory
