#include "life_core.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace life {

namespace {

constexpr uint64_t kMixIncrement = 0x9e3779b97f4a7c15ULL;
constexpr uint64_t kMixMultiplier1 = 0xbf58476d1ce4e5b9ULL;
constexpr uint64_t kMixMultiplier2 = 0x94d049bb133111ebULL;

uint64_t mix64(uint64_t value) {
    value += kMixIncrement;
    value = (value ^ (value >> 30)) * kMixMultiplier1;
    value = (value ^ (value >> 27)) * kMixMultiplier2;
    return value ^ (value >> 31);
}

uint8_t nextState(int neighbours, uint8_t alive) {
    return neighbours == 3 || (neighbours == 2 && alive != 0) ? 1 : 0;
}

int neighbourAt(const uint8_t* row, int64_t column, uint64_t width, bool wrap) {
    if (column < 0) {
        if (!wrap) {
            return 0;
        }
        column += static_cast<int64_t>(width);
    } else if (column >= static_cast<int64_t>(width)) {
        if (!wrap) {
            return 0;
        }
        column -= static_cast<int64_t>(width);
    }
    return row[column];
}

void computeRange(const Grid& src, Grid& dst, bool wrap, uint64_t begin, uint64_t end) {
    const uint64_t width = src.width;
    for (uint64_t rowIndex = begin; rowIndex < end; ++rowIndex) {
        const uint8_t* up = src.data + rowIndex * src.stride;
        const uint8_t* mid = up + src.stride;
        const uint8_t* down = mid + src.stride;
        uint8_t* out = dst.data + (rowIndex + 1) * dst.stride;
        if (width == 1) {
            int neighbours = 0;
            neighbours += neighbourAt(up, -1, width, wrap);
            neighbours += up[0];
            neighbours += neighbourAt(up, 1, width, wrap);
            neighbours += neighbourAt(mid, -1, width, wrap);
            neighbours += neighbourAt(mid, 1, width, wrap);
            neighbours += neighbourAt(down, -1, width, wrap);
            neighbours += down[0];
            neighbours += neighbourAt(down, 1, width, wrap);
            out[0] = nextState(neighbours, mid[0]);
            continue;
        }
        if (wrap) {
            int neighbours = up[width - 1] + up[0] + up[1] + mid[width - 1] + mid[1] + down[width - 1] + down[0] +
                             down[1];
            out[0] = nextState(neighbours, mid[0]);
            for (uint64_t column = 1; column + 1 < width; ++column) {
                neighbours = up[column - 1] + up[column] + up[column + 1] + mid[column - 1] + mid[column + 1] +
                             down[column - 1] + down[column] + down[column + 1];
                out[column] = nextState(neighbours, mid[column]);
            }
            neighbours = up[width - 2] + up[width - 1] + up[0] + mid[width - 2] + mid[0] + down[width - 2] +
                         down[width - 1] + down[0];
            out[width - 1] = nextState(neighbours, mid[width - 1]);
        } else {
            int neighbours = up[0] + up[1] + mid[1] + down[0] + down[1];
            out[0] = nextState(neighbours, mid[0]);
            for (uint64_t column = 1; column + 1 < width; ++column) {
                neighbours = up[column - 1] + up[column] + up[column + 1] + mid[column - 1] + mid[column + 1] +
                             down[column - 1] + down[column] + down[column + 1];
                out[column] = nextState(neighbours, mid[column]);
            }
            neighbours = up[width - 2] + up[width - 1] + mid[width - 2] + down[width - 2] + down[width - 1];
            out[width - 1] = nextState(neighbours, mid[width - 1]);
        }
    }
}

}

uint64_t roundUp(uint64_t value, uint64_t multiple) {
    if (multiple == 0) {
        return value;
    }
    return (value + multiple - 1) / multiple * multiple;
}

uint64_t displaySampleRow(uint64_t displayRow, uint64_t displayHeight, uint64_t globalHeight) {
    uint64_t firstRow = displayRow * globalHeight / displayHeight;
    uint64_t lastRow = (displayRow + 1) * globalHeight / displayHeight;
    if (lastRow <= firstRow) {
        lastRow = firstRow + 1;
    }
    uint64_t globalRow = (firstRow + lastRow) / 2;
    if (globalRow >= globalHeight) {
        globalRow = globalHeight - 1;
    }
    return globalRow;
}

void fillOwnRows(const Grid& grid, uint64_t globalRowStart, uint64_t seed, uint32_t densityPercent) {
    for (uint64_t rowIndex = 0; rowIndex < grid.rows; ++rowIndex) {
        const uint64_t globalRow = globalRowStart + rowIndex;
        uint8_t* row = grid.data + (rowIndex + 1) * grid.stride;
        uint64_t state = seed ^ (globalRow * kMixIncrement);
        for (uint64_t column = 0; column < grid.width; ++column) {
            state += kMixIncrement;
            const uint64_t value = mix64(state);
            row[column] = value % 100 < densityPercent ? 1 : 0;
        }
    }
}

void computeAll(const Grid& src, Grid& dst, bool wrap, int threads) {
    if (src.rows == 0) {
        return;
    }
    int workerCount = std::max(1, threads);
    if (static_cast<uint64_t>(workerCount) > src.rows) {
        workerCount = static_cast<int>(src.rows);
    }
    if (workerCount == 1) {
        computeRange(src, dst, wrap, 0, src.rows);
        return;
    }
    const uint64_t chunk = (src.rows + static_cast<uint64_t>(workerCount) - 1) / static_cast<uint64_t>(workerCount);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workerCount));
    for (int worker = 0; worker < workerCount; ++worker) {
        const uint64_t begin = static_cast<uint64_t>(worker) * chunk;
        const uint64_t end = std::min(src.rows, begin + chunk);
        if (begin >= end) {
            continue;
        }
        workers.emplace_back([&src, &dst, wrap, begin, end] { computeRange(src, dst, wrap, begin, end); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

void checksumOwnRows(const Grid& grid, uint64_t globalRowStart, uint64_t& outLiveCells, uint64_t& outHash) {
    uint64_t liveCells = 0;
    uint64_t hash = 0;
    for (uint64_t rowIndex = 0; rowIndex < grid.rows; ++rowIndex) {
        const uint64_t globalRow = globalRowStart + rowIndex;
        const uint8_t* row = grid.data + (rowIndex + 1) * grid.stride;
        for (uint64_t column = 0; column < grid.width; ++column) {
            if (row[column] == 0) {
                continue;
            }
            ++liveCells;
            hash += mix64(globalRow * kMixIncrement ^ column);
        }
    }
    outLiveCells = liveCells;
    outHash = hash;
}

void downsampleOwnRows(const Grid& grid, uint8_t* display, uint64_t displayWidth, uint64_t displayHeight,
                       uint64_t displayRowStart, uint64_t displayRowEnd, uint64_t globalRowStart,
                       uint64_t globalHeight) {
    if (grid.rows == 0 || displayWidth == 0 || displayHeight == 0 || displayRowStart >= displayRowEnd) {
        return;
    }
    for (uint64_t displayRow = displayRowStart; displayRow < displayRowEnd; ++displayRow) {
        const uint64_t globalRow = displaySampleRow(displayRow, displayHeight, globalHeight);
        if (globalRow < globalRowStart || globalRow >= globalRowStart + grid.rows) {
            continue;
        }
        const uint64_t ownRow = globalRow - globalRowStart;
        const uint8_t* source = grid.data + (ownRow + 1) * grid.stride;
        uint8_t* destination = display + displayRow * displayWidth;
        for (uint64_t displayColumn = 0; displayColumn < displayWidth; ++displayColumn) {
            uint64_t firstColumn = displayColumn * grid.width / displayWidth;
            uint64_t lastColumn = (displayColumn + 1) * grid.width / displayWidth;
            if (lastColumn <= firstColumn) {
                lastColumn = firstColumn + 1;
            }
            uint64_t column = (firstColumn + lastColumn) / 2;
            if (column >= grid.width) {
                column = grid.width - 1;
            }
            destination[displayColumn] = source[column];
        }
    }
}

}
