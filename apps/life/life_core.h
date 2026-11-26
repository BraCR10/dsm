#ifndef DSM_APPS_LIFE_LIFE_CORE_H
#define DSM_APPS_LIFE_LIFE_CORE_H

#include <cstdint>

namespace life {

struct Grid {
    uint8_t* data = nullptr;
    uint64_t stride = 0;
    uint64_t width = 0;
    uint64_t rows = 0;
};

uint64_t roundUp(uint64_t value, uint64_t multiple);
uint64_t displaySampleRow(uint64_t displayRow, uint64_t displayHeight, uint64_t globalHeight);
void fillOwnRows(const Grid& grid, uint64_t globalRowStart, uint64_t seed, uint32_t densityPercent);
void computeAll(const Grid& src, Grid& dst, bool wrap, int threads);
void checksumOwnRows(const Grid& grid, uint64_t globalRowStart, uint64_t& outLiveCells, uint64_t& outHash);
void downsampleOwnRows(const Grid& grid, uint8_t* display, uint64_t displayWidth, uint64_t displayHeight,
                       uint64_t displayRowStart, uint64_t displayRowEnd, uint64_t globalRowStart,
                       uint64_t globalHeight);

}

#endif
