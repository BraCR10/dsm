#ifndef DSM_STATS_STATS_MESSAGES_H
#define DSM_STATS_STATS_MESSAGES_H

#include <cstdint>
#include <optional>
#include <vector>

#include "dsm/dsm.h"

namespace dsm::stats {

std::vector<uint8_t> encodeStats(const DsmStats& stats);
std::optional<DsmStats> decodeStats(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> encodePageTable(const std::vector<PageTableEntry>& entries);
std::optional<std::vector<PageTableEntry>> decodePageTable(const std::vector<uint8_t>& bytes);

}  // namespace dsm::stats

#endif
