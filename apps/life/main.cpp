#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/run_stats.h"
#include "../common/worker_claim.h"
#include "dsm/dsm.h"
#include "life_core.h"

namespace {

constexpr const char* kDefaultConfigPath = "config/cluster.json";
constexpr uint64_t kDescriptorMagic = 0x4c49464544534d31ULL;
constexpr uint32_t kDescriptorVersion = 2;
constexpr uint32_t kMaximumWorkers = 8;
constexpr uint64_t kPageSize = 4096;

struct WorkerSlot {
    uint64_t bufA = 0;
    uint64_t bufB = 0;
    uint64_t display = 0;
    uint64_t rowStart = 0;
    uint64_t rowCount = 0;
    uint64_t displayRowStart = 0;
    uint64_t displayRowEnd = 0;
    uint64_t liveCells = 0;
    uint64_t checksum = 0;
    uint64_t elapsedMicros = 0;
    uint64_t pageFaults = 0;
    uint64_t pagesSent = 0;
    uint64_t pagesReceived = 0;
    uint64_t invalidations = 0;
};

struct Descriptor {
    uint64_t magic = 0;
    uint32_t version = 0;
    uint32_t workerCount = 0;
    int32_t barrierId = -1;
    int32_t mutexId = -1;
    uint32_t joinedWorkers = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t stride = 0;
    uint64_t generations = 0;
    uint64_t snapshotEvery = 0;
    uint64_t seed = 0;
    uint32_t density = 0;
    uint32_t wrap = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    WorkerSlot workers[kMaximumWorkers];
};

static_assert(sizeof(Descriptor) <= kPageSize, "descriptor must fit in one page");

struct Options {
    std::string configPath = kDefaultConfigPath;
    int nodeId = -1;
    uint32_t workers = 1;
    uint64_t width = 1024;
    uint64_t height = 1024;
    uint64_t generations = 100;
    uint64_t snapshotEvery = 0;
    uint64_t seed = 12345;
    uint32_t density = 30;
    int threads = 1;
    uint64_t displayWidth = 512;
    uint64_t displayHeight = 512;
    uint64_t joinAddress = 0;
    std::string ppmPrefix;
    bool ascii = false;
    bool clear = false;
    bool wrap = false;
    bool creatorOptionGiven = false;
    bool help = false;
};

int defaultThreadCount() {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 1;
    }
    return static_cast<int>(std::min(hardware, 8u));
}

void printUsage() {
    std::printf(
        "Usage:\n"
        "  life [--config <path>] [--node <id>] [--workers <n>] [--width <w>] [--height <h>]\n"
        "       [--generations <g>] [--snapshot-every <k>] [--seed <s>] [--density <percent>]\n"
        "       [--threads <t>] [--display-width <w>] [--display-height <h>] [--ppm <prefix>]\n"
        "       [--ascii] [--clear] [--wrap]\n"
        "  life --join <descriptor_address> [--config <path>] [--node <id>] [--threads <t>]\n"
        "\n"
        "The process started without --join creates the grid, becomes worker 1 and prints the\n"
        "join command. Run that command once per remaining worker, on any machine of the cluster.\n"
        "--node picks the local DSM node only when several run on the same machine.\n"
        "Example:\n"
        "  life --workers 3 --width 32768 --height 32768 --generations 100 --ascii\n"
        "  life --join 0x100000000000 --threads 4\n");
}

bool parseU64(const char* text, uint64_t& outValue) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    outValue = static_cast<uint64_t>(value);
    return true;
}

bool parseInt(const char* text, int& outValue) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    outValue = static_cast<int>(value);
    return true;
}

bool isCreatorOption(const std::string& argument) {
    static const char* const creatorOptions[] = {
        "--workers", "--width",          "--height",         "--generations", "--snapshot-every", "--seed",
        "--density", "--display-width", "--display-height", "--ppm",         "--ascii",          "--clear",
        "--wrap"};
    for (const char* option : creatorOptions) {
        if (argument == option) {
            return true;
        }
    }
    return false;
}

bool parseOptions(int argumentCount, char** arguments, Options& options) {
    options.threads = defaultThreadCount();
    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        const auto valueFor = [&](const char* name) -> const char* {
            if (index + 1 >= argumentCount) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return arguments[++index];
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            return true;
        }
        if (isCreatorOption(argument)) {
            options.creatorOptionGiven = true;
        }
        if (argument == "--config") {
            const char* value = valueFor("--config");
            if (value == nullptr) {
                return false;
            }
            options.configPath = value;
        } else if (argument == "--node") {
            const char* value = valueFor("--node");
            if (value == nullptr || !parseInt(value, options.nodeId)) {
                std::fprintf(stderr, "invalid --node value\n");
                return false;
            }
        } else if (argument == "--workers") {
            uint64_t value = 0;
            const char* text = valueFor("--workers");
            if (text == nullptr || !parseU64(text, value) || value == 0 || value > kMaximumWorkers) {
                std::fprintf(stderr, "invalid --workers value\n");
                return false;
            }
            options.workers = static_cast<uint32_t>(value);
        } else if (argument == "--width") {
            const char* text = valueFor("--width");
            if (text == nullptr || !parseU64(text, options.width) || options.width == 0 || options.width > UINT32_MAX) {
                std::fprintf(stderr, "invalid --width value\n");
                return false;
            }
        } else if (argument == "--height") {
            const char* text = valueFor("--height");
            if (text == nullptr || !parseU64(text, options.height) || options.height == 0 ||
                options.height > UINT32_MAX) {
                std::fprintf(stderr, "invalid --height value\n");
                return false;
            }
        } else if (argument == "--generations") {
            const char* text = valueFor("--generations");
            if (text == nullptr || !parseU64(text, options.generations)) {
                std::fprintf(stderr, "invalid --generations value\n");
                return false;
            }
        } else if (argument == "--snapshot-every") {
            const char* text = valueFor("--snapshot-every");
            if (text == nullptr || !parseU64(text, options.snapshotEvery)) {
                std::fprintf(stderr, "invalid --snapshot-every value\n");
                return false;
            }
        } else if (argument == "--seed") {
            const char* text = valueFor("--seed");
            if (text == nullptr || !parseU64(text, options.seed)) {
                std::fprintf(stderr, "invalid --seed value\n");
                return false;
            }
        } else if (argument == "--density") {
            uint64_t value = 0;
            const char* text = valueFor("--density");
            if (text == nullptr || !parseU64(text, value) || value > 100) {
                std::fprintf(stderr, "invalid --density value\n");
                return false;
            }
            options.density = static_cast<uint32_t>(value);
        } else if (argument == "--threads") {
            const char* text = valueFor("--threads");
            if (text == nullptr || !parseInt(text, options.threads) || options.threads < 1) {
                std::fprintf(stderr, "invalid --threads value\n");
                return false;
            }
        } else if (argument == "--display-width") {
            const char* text = valueFor("--display-width");
            if (text == nullptr || !parseU64(text, options.displayWidth) || options.displayWidth == 0 ||
                options.displayWidth > UINT32_MAX) {
                std::fprintf(stderr, "invalid --display-width value\n");
                return false;
            }
        } else if (argument == "--display-height") {
            const char* text = valueFor("--display-height");
            if (text == nullptr || !parseU64(text, options.displayHeight) || options.displayHeight == 0 ||
                options.displayHeight > UINT32_MAX) {
                std::fprintf(stderr, "invalid --display-height value\n");
                return false;
            }
        } else if (argument == "--join") {
            const char* text = valueFor("--join");
            if (text == nullptr || !parseU64(text, options.joinAddress) || options.joinAddress == 0) {
                std::fprintf(stderr, "invalid --join value\n");
                return false;
            }
        } else if (argument == "--ppm") {
            const char* value = valueFor("--ppm");
            if (value == nullptr) {
                return false;
            }
            options.ppmPrefix = value;
        } else if (argument == "--ascii") {
            options.ascii = true;
        } else if (argument == "--clear") {
            options.clear = true;
        } else if (argument == "--wrap") {
            options.wrap = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argument.c_str());
            return false;
        }
    }
    return true;
}

bool validateOptions(const Options& options) {
    if (options.joinAddress != 0) {
        if (options.creatorOptionGiven) {
            std::fprintf(stderr, "grid and output options only apply to the process that creates the run\n");
            return false;
        }
        return true;
    }
    if (options.workers == 0 || options.workers > kMaximumWorkers) {
        std::fprintf(stderr, "--workers must be between 1 and %u\n", kMaximumWorkers);
        return false;
    }
    if (options.height < options.workers) {
        std::fprintf(stderr, "--height must be at least --workers\n");
        return false;
    }
    if (options.width == 0 || options.height == 0) {
        std::fprintf(stderr, "--width and --height must be greater than zero\n");
        return false;
    }
    return true;
}

bool syncBarrier(int barrierId) {
    const int status = dsm_barrier(barrierId);
    if (status < 0) {
        std::fprintf(stderr, "dsm_barrier failed with status %d\n", status);
        return false;
    }
    return true;
}

Descriptor* descriptorFromAddress(uint64_t address) {
    return reinterpret_cast<Descriptor*>(static_cast<uintptr_t>(address));
}

uint8_t* bufferFromAddress(uint64_t address) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
}

const uint8_t* readBufferFromAddress(uint64_t address) {
    return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address));
}

bool allocateWorker(Descriptor* descriptor, int workerIndex) {
    WorkerSlot& slot = descriptor->workers[workerIndex];
    const uint64_t bufferBytes = (slot.rowCount + 2) * descriptor->stride;
    const uint64_t displayBytes = static_cast<uint64_t>(descriptor->displayWidth) * descriptor->displayHeight;
    uint8_t* first = static_cast<uint8_t*>(dsm_malloc(bufferBytes));
    uint8_t* second = static_cast<uint8_t*>(dsm_malloc(bufferBytes));
    uint8_t* display = static_cast<uint8_t*>(dsm_malloc(displayBytes));
    if (first == nullptr || second == nullptr || display == nullptr) {
        std::fprintf(stderr, "dsm_malloc failed: %s\n", std::strerror(errno));
        if (first != nullptr) {
            dsm_free(first);
        }
        if (second != nullptr) {
            dsm_free(second);
        }
        if (display != nullptr) {
            dsm_free(display);
        }
        return false;
    }
    std::memset(first, 0, bufferBytes);
    std::memset(second, 0, bufferBytes);
    std::memset(display, 0, displayBytes);
    slot.bufA = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(first));
    slot.bufB = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(second));
    slot.display = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(display));
    const life::Grid grid{first, descriptor->stride, descriptor->width, slot.rowCount};
    life::fillOwnRows(grid, slot.rowStart, descriptor->seed, descriptor->density);
    return true;
}

bool exchangeHalos(Descriptor* descriptor, int workerIndex, bool neighbourUsesA, uint8_t* localBase) {
    const WorkerSlot& slot = descriptor->workers[workerIndex];
    const uint64_t stride = descriptor->stride;
    const uint64_t width = descriptor->width;
    const uint64_t rows = slot.rowCount;
    if (rows == 0) {
        return true;
    }
    const uint64_t aboveIndex = workerIndex > 0 ? static_cast<uint64_t>(workerIndex - 1)
                                           : static_cast<uint64_t>(descriptor->workerCount - 1);
    const bool aboveExists = workerIndex > 0 || descriptor->wrap != 0;
    if (aboveExists) {
        const WorkerSlot& above = descriptor->workers[aboveIndex];
        if (above.rowCount > 0) {
            const uint8_t* source = readBufferFromAddress(neighbourUsesA ? above.bufA : above.bufB);
            std::memcpy(localBase, source + above.rowCount * stride, width);
        } else {
            std::memset(localBase, 0, width);
        }
    } else {
        std::memset(localBase, 0, width);
    }
    const uint64_t belowIndex = workerIndex + 1 < static_cast<int>(descriptor->workerCount)
                                    ? static_cast<uint64_t>(workerIndex + 1)
                                    : 0;
    const bool belowExists = workerIndex + 1 < static_cast<int>(descriptor->workerCount) || descriptor->wrap != 0;
    uint8_t* bottom = localBase + (rows + 1) * stride;
    if (belowExists) {
        const WorkerSlot& below = descriptor->workers[belowIndex];
        if (below.rowCount > 0) {
            const uint8_t* source = readBufferFromAddress(neighbourUsesA ? below.bufA : below.bufB);
            std::memcpy(bottom, source + stride, width);
        } else {
            std::memset(bottom, 0, width);
        }
    } else {
        std::memset(bottom, 0, width);
    }
    return true;
}

bool writePpm(const Descriptor& descriptor, const std::vector<uint8_t>& frame, uint64_t generation,
              const std::string& prefix) {
    char name[1024];
    const int length = std::snprintf(name, sizeof(name), "%s_%05llu.ppm", prefix.c_str(),
                                     static_cast<unsigned long long>(generation));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(name)) {
        std::fprintf(stderr, "ppm path is too long\n");
        return false;
    }
    std::ofstream output(name, std::ios::binary);
    if (!output) {
        std::fprintf(stderr, "cannot open %s\n", name);
        return false;
    }
    output << "P6\n" << descriptor.displayWidth << " " << descriptor.displayHeight << "\n255\n";
    std::vector<uint8_t> row(static_cast<size_t>(descriptor.displayWidth) * 3);
    for (uint32_t y = 0; y < descriptor.displayHeight; ++y) {
        for (uint32_t x = 0; x < descriptor.displayWidth; ++x) {
            const uint8_t value =
                frame[static_cast<size_t>(y) * descriptor.displayWidth + x] != 0 ? 255 : 0;
            row[static_cast<size_t>(x) * 3] = value;
            row[static_cast<size_t>(x) * 3 + 1] = value;
            row[static_cast<size_t>(x) * 3 + 2] = value;
        }
        output.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    if (!output) {
        std::fprintf(stderr, "cannot write %s\n", name);
        return false;
    }
    return true;
}

void writeAscii(const Descriptor& descriptor, const std::vector<uint8_t>& frame, uint64_t generation, bool clear) {
    const std::ptrdiff_t live = std::count(frame.begin(), frame.end(), static_cast<uint8_t>(1));
    if (clear) {
        std::fputs("\033[H\033[2J", stdout);
    }
    std::printf("generation %llu, live %llu\n", static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(live));
    constexpr int kAsciiWidth = 80;
    constexpr int kAsciiHeight = 24;
    const char* ramp = " .:-=+*#%@";
    for (int asciiRow = 0; asciiRow < kAsciiHeight; ++asciiRow) {
        const uint64_t firstRow = static_cast<uint64_t>(asciiRow) * descriptor.displayHeight / kAsciiHeight;
        uint64_t lastRow = static_cast<uint64_t>(asciiRow + 1) * descriptor.displayHeight / kAsciiHeight;
        if (lastRow <= firstRow) {
            lastRow = firstRow + 1;
        }
        if (lastRow > descriptor.displayHeight) {
            lastRow = descriptor.displayHeight;
        }
        for (int asciiColumn = 0; asciiColumn < kAsciiWidth; ++asciiColumn) {
            const uint64_t firstColumn =
                static_cast<uint64_t>(asciiColumn) * descriptor.displayWidth / kAsciiWidth;
            uint64_t lastColumn =
                static_cast<uint64_t>(asciiColumn + 1) * descriptor.displayWidth / kAsciiWidth;
            if (lastColumn <= firstColumn) {
                lastColumn = firstColumn + 1;
            }
            if (lastColumn > descriptor.displayWidth) {
                lastColumn = descriptor.displayWidth;
            }
            uint64_t liveInBlock = 0;
            uint64_t totalInBlock = 0;
            for (uint64_t row = firstRow; row < lastRow; ++row) {
                for (uint64_t column = firstColumn; column < lastColumn; ++column) {
                    liveInBlock += frame[static_cast<size_t>(row) * descriptor.displayWidth + column] != 0 ? 1 : 0;
                    ++totalInBlock;
                }
            }
            const double ratio =
                totalInBlock == 0 ? 0.0 : static_cast<double>(liveInBlock) / static_cast<double>(totalInBlock);
            int rampIndex = static_cast<int>(ratio * 10.0);
            if (rampIndex > 9) {
                rampIndex = 9;
            }
            std::putchar(ramp[rampIndex]);
        }
        std::putchar('\n');
    }
    std::fflush(stdout);
}

std::vector<uint8_t> assembleFrame(const Descriptor& descriptor) {
    std::vector<uint8_t> frame(static_cast<size_t>(descriptor.displayWidth) * descriptor.displayHeight, 0);
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const WorkerSlot& slot = descriptor.workers[worker];
        if (slot.displayRowEnd <= slot.displayRowStart) {
            continue;
        }
        const uint8_t* source = readBufferFromAddress(slot.display);
        for (uint64_t row = slot.displayRowStart; row < slot.displayRowEnd; ++row) {
            std::memcpy(frame.data() + static_cast<size_t>(row) * descriptor.displayWidth,
                        source + static_cast<size_t>(row) * descriptor.displayWidth, descriptor.displayWidth);
        }
    }
    return frame;
}

bool snapshot(Descriptor* descriptor, int workerIndex, const life::Grid& grid, uint64_t generation,
              const Options& options) {
    WorkerSlot& slot = descriptor->workers[workerIndex];
    uint8_t* display = bufferFromAddress(slot.display);
    life::downsampleOwnRows(grid, display, descriptor->displayWidth, descriptor->displayHeight, slot.displayRowStart,
                            slot.displayRowEnd, slot.rowStart, descriptor->height);
    if (!syncBarrier(descriptor->barrierId)) {
        return false;
    }
    if (workerIndex == 0 && (!options.ppmPrefix.empty() || options.ascii)) {
        const std::vector<uint8_t> frame = assembleFrame(*descriptor);
        if (!options.ppmPrefix.empty()) {
            writePpm(*descriptor, frame, generation, options.ppmPrefix);
        }
        if (options.ascii) {
            writeAscii(*descriptor, frame, generation, options.clear);
        }
    }
    return syncBarrier(descriptor->barrierId);
}

void printSummary(const Descriptor& descriptor) {
    uint64_t totalLive = 0;
    uint64_t totalHash = 0;
    uint64_t maximumElapsed = 0;
    std::printf("Life finished: %u worker(s), %u x %u grid, %llu generation(s)\n",
                static_cast<unsigned>(descriptor.workerCount), static_cast<unsigned>(descriptor.width),
                static_cast<unsigned>(descriptor.height), static_cast<unsigned long long>(descriptor.generations));
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const WorkerSlot& slot = descriptor.workers[worker];
        totalLive += slot.liveCells;
        totalHash += slot.checksum;
        maximumElapsed = std::max(maximumElapsed, slot.elapsedMicros);
        std::printf(
            "  worker %u rows [%llu, %llu) live %llu checksum %016llx faults %llu sent %llu recv %llu "
            "invalidations %llu time %.3f s\n",
            apps::workerNumber(worker), static_cast<unsigned long long>(slot.rowStart),
            static_cast<unsigned long long>(slot.rowStart + slot.rowCount),
            static_cast<unsigned long long>(slot.liveCells), static_cast<unsigned long long>(slot.checksum),
            static_cast<unsigned long long>(slot.pageFaults), static_cast<unsigned long long>(slot.pagesSent),
            static_cast<unsigned long long>(slot.pagesReceived),
            static_cast<unsigned long long>(slot.invalidations),
            static_cast<double>(slot.elapsedMicros) / 1000000.0);
    }
    std::printf("  total live %llu, fingerprint %016llx\n", static_cast<unsigned long long>(totalLive),
                static_cast<unsigned long long>(totalHash));
    if (maximumElapsed > 0) {
        const double cells = static_cast<double>(descriptor.width) * descriptor.height * descriptor.generations;
        const double seconds = static_cast<double>(maximumElapsed) / 1000000.0;
        std::printf("  aggregate %.3f Gcells/s\n", cells / seconds / 1000000000.0);
    }
}

int runSimulation(Descriptor* descriptor, int workerIndex, const Options& options) {
    WorkerSlot& slot = descriptor->workers[workerIndex];
    uint8_t* firstBuffer = bufferFromAddress(slot.bufA);
    uint8_t* secondBuffer = bufferFromAddress(slot.bufB);
    uint8_t* display = bufferFromAddress(slot.display);
    life::Grid firstGrid{firstBuffer, descriptor->stride, descriptor->width, slot.rowCount};
    life::Grid secondGrid{secondBuffer, descriptor->stride, descriptor->width, slot.rowCount};
    life::Grid* source = &firstGrid;
    life::Grid* destination = &secondGrid;
    const DsmStats statsBefore = apps::readNodeStats();
    const auto start = std::chrono::steady_clock::now();
    bool ok = true;
    bool finalSnapshotTaken = false;
    for (uint64_t generation = 0; generation < descriptor->generations && ok; ++generation) {
        life::computeAll(*source, *destination, descriptor->wrap != 0, options.threads);
        if (!syncBarrier(descriptor->barrierId)) {
            ok = false;
            break;
        }
        exchangeHalos(descriptor, workerIndex, generation % 2 == 1, destination->data);
        std::swap(source, destination);
        const uint64_t completed = generation + 1;
        if (descriptor->snapshotEvery > 0 && completed % descriptor->snapshotEvery == 0) {
            if (!snapshot(descriptor, workerIndex, *source, completed, options)) {
                ok = false;
                break;
            }
            if (completed == descriptor->generations) {
                finalSnapshotTaken = true;
            }
        }
    }
    if (ok && !finalSnapshotTaken) {
        if (!snapshot(descriptor, workerIndex, *source, descriptor->generations, options)) {
            ok = false;
        }
    }
    uint64_t liveCells = 0;
    uint64_t checksum = 0;
    life::checksumOwnRows(*source, slot.rowStart, liveCells, checksum);
    const apps::RunStats stats = apps::statsSince(statsBefore);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    slot.liveCells = liveCells;
    slot.checksum = checksum;
    slot.elapsedMicros = static_cast<uint64_t>(elapsed.count());
    slot.pageFaults = stats.pageFaults;
    slot.pagesSent = stats.pagesSent;
    slot.pagesReceived = stats.pagesReceived;
    slot.invalidations = stats.invalidations;
    if (ok && !syncBarrier(descriptor->barrierId)) {
        ok = false;
    }
    if (ok && workerIndex == 0) {
        printSummary(*descriptor);
    }
    if (ok && !syncBarrier(descriptor->barrierId)) {
        ok = false;
    }
    dsm_free(firstBuffer);
    dsm_free(secondBuffer);
    dsm_free(display);
    return ok ? 0 : 1;
}

int runCreator(const Options& options) {
    Descriptor* descriptor = static_cast<Descriptor*>(dsm_malloc(sizeof(Descriptor)));
    if (descriptor == nullptr) {
        std::fprintf(stderr, "dsm_malloc failed: %s\n", std::strerror(errno));
        return 1;
    }
    *descriptor = Descriptor{};
    descriptor->magic = kDescriptorMagic;
    descriptor->version = kDescriptorVersion;
    descriptor->workerCount = options.workers;
    descriptor->width = static_cast<uint32_t>(options.width);
    descriptor->height = static_cast<uint32_t>(options.height);
    descriptor->stride = life::roundUp(options.width, kPageSize);
    descriptor->generations = options.generations;
    descriptor->snapshotEvery = options.snapshotEvery;
    descriptor->seed = options.seed;
    descriptor->density = options.density;
    descriptor->wrap = options.wrap ? 1 : 0;
    descriptor->displayWidth = static_cast<uint32_t>(options.displayWidth);
    descriptor->displayHeight = static_cast<uint32_t>(options.displayHeight);
    if (descriptor->displayWidth > descriptor->width) {
        descriptor->displayWidth = descriptor->width;
    }
    if (descriptor->displayHeight > descriptor->height) {
        descriptor->displayHeight = descriptor->height;
    }
    descriptor->barrierId = dsm_barrier_init(static_cast<int>(descriptor->workerCount));
    if (descriptor->barrierId < 0) {
        std::fprintf(stderr, "dsm_barrier_init failed with status %d\n", descriptor->barrierId);
        dsm_free(descriptor);
        return 1;
    }
    descriptor->mutexId = dsm_mutex_init();
    if (descriptor->mutexId < 0) {
        std::fprintf(stderr, "dsm_mutex_init failed with status %d\n", descriptor->mutexId);
        dsm_free(descriptor);
        return 1;
    }
    descriptor->joinedWorkers = 1;
    const uint64_t baseRows = options.height / options.workers;
    const uint64_t remainder = options.height % options.workers;
    uint64_t rowStart = 0;
    for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
        const uint64_t rows = baseRows + (worker < remainder ? 1 : 0);
        WorkerSlot& slot = descriptor->workers[worker];
        slot.rowStart = rowStart;
        slot.rowCount = rows;
        slot.displayRowStart = descriptor->displayHeight;
        slot.displayRowEnd = descriptor->displayHeight;
        for (uint64_t displayRow = 0; displayRow < descriptor->displayHeight; ++displayRow) {
            const uint64_t sourceRow =
                life::displaySampleRow(displayRow, descriptor->displayHeight, descriptor->height);
            if (sourceRow >= slot.rowStart && sourceRow < slot.rowStart + slot.rowCount) {
                if (slot.displayRowStart == descriptor->displayHeight) {
                    slot.displayRowStart = displayRow;
                }
                slot.displayRowEnd = displayRow + 1;
            }
        }
        rowStart += rows;
    }
    std::printf("Life grid %u x %u, %u worker(s), %llu generation(s), seed %llu, wrap %s\n",
                static_cast<unsigned>(descriptor->width), static_cast<unsigned>(descriptor->height),
                static_cast<unsigned>(descriptor->workerCount),
                static_cast<unsigned long long>(descriptor->generations),
                static_cast<unsigned long long>(descriptor->seed), descriptor->wrap != 0 ? "yes" : "no");
    if (descriptor->workerCount > 1) {
        std::printf("Run this command %u more time(s), on any machine of the cluster:\n",
                    static_cast<unsigned>(descriptor->workerCount - 1));
        std::printf("  life --join 0x%llx --threads %d",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(descriptor)), options.threads);
        if (options.configPath != kDefaultConfigPath) {
            std::printf(" --config %s", options.configPath.c_str());
        }
        std::printf("\n");
    }
    std::fflush(stdout);
    if (!allocateWorker(descriptor, 0)) {
        dsm_free(descriptor);
        return 1;
    }
    if (!syncBarrier(descriptor->barrierId)) {
        dsm_free(descriptor);
        return 1;
    }
    dsm_mutex_destroy(descriptor->mutexId);
    {
        WorkerSlot& slot = descriptor->workers[0];
        if (!exchangeHalos(descriptor, 0, true, bufferFromAddress(slot.bufA))) {
            dsm_free(descriptor);
            return 1;
        }
        if (!syncBarrier(descriptor->barrierId)) {
            dsm_free(descriptor);
            return 1;
        }
    }
    const int result = runSimulation(descriptor, 0, options);
    dsm_free(descriptor);
    return result;
}

int runJoiner(const Options& options) {
    Descriptor* descriptor = descriptorFromAddress(options.joinAddress);
    if (descriptor->magic != kDescriptorMagic || descriptor->version != kDescriptorVersion) {
        std::fprintf(stderr, "address 0x%llx is not a life descriptor\n",
                     static_cast<unsigned long long>(options.joinAddress));
        return 1;
    }
    if (descriptor->workerCount == 0 || descriptor->workerCount > kMaximumWorkers) {
        std::fprintf(stderr, "descriptor has an invalid worker count\n");
        return 1;
    }
    if (descriptor->barrierId < 0 || descriptor->mutexId < 0) {
        std::fprintf(stderr, "descriptor has an invalid barrier or mutex\n");
        return 1;
    }
    uint32_t workerIndex = 0;
    if (!apps::claimWorkerIndex(descriptor->mutexId, descriptor->joinedWorkers, descriptor->workerCount,
                                workerIndex)) {
        return 1;
    }
    std::printf("Joined as worker %u of %u\n", apps::workerNumber(workerIndex),
                static_cast<unsigned>(descriptor->workerCount));
    std::fflush(stdout);
    const int worker = static_cast<int>(workerIndex);
    if (!allocateWorker(descriptor, worker)) {
        return 1;
    }
    if (!syncBarrier(descriptor->barrierId)) {
        return 1;
    }
    {
        WorkerSlot& slot = descriptor->workers[worker];
        if (!exchangeHalos(descriptor, worker, true, bufferFromAddress(slot.bufA))) {
            return 1;
        }
        if (!syncBarrier(descriptor->barrierId)) {
            return 1;
        }
    }
    return runSimulation(descriptor, worker, options);
}

}

int main(int argumentCount, char** arguments) {
    Options options;
    if (!parseOptions(argumentCount, arguments, options)) {
        printUsage();
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }
    if (!validateOptions(options)) {
        return 2;
    }
    const int status = dsm_init(options.configPath.c_str(), options.nodeId);
    if (status == DSM_ERR_NODE_UNAVAILABLE) {
        std::printf("No DSM node is running on this machine. Start one with dsm_node.\n");
        return 1;
    }
    if (status == DSM_ERR_INVALID_ARGUMENT) {
        std::printf("The requested node is not in the configuration or does not run on this machine.\n");
        return 1;
    }
    if (status != DSM_OK) {
        std::printf("dsm_init failed with status %d\n", status);
        return 1;
    }
    const int result = options.joinAddress != 0 ? runJoiner(options) : runCreator(options);
    dsm_finalize();
    return result;
}
