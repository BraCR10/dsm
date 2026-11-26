#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../common/run_stats.h"
#include "../common/worker_claim.h"
#include "compress_core.h"
#include "dsm/dsm.h"

namespace {

constexpr const char* kDefaultConfigPath = "config/cluster.json";
constexpr uint64_t kDescriptorMagic = 0x44534d434f4d5031ULL;
constexpr uint32_t kDescriptorVersion = 2;
constexpr uint32_t kMaximumWorkers = 16;
constexpr uint64_t kDefaultBlockSize = 65536;
constexpr uint64_t kFileVersion = 1;
constexpr char kFileMagic[8] = {'D', 'S', 'M', 'C', 'M', 'P', '0', '1'};

constexpr uint32_t kModeCompress = 1;
constexpr uint32_t kModeDecompress = 2;

struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t codec;
    uint64_t rawSize;
    uint64_t blockSize;
    uint64_t blockCount;
    uint64_t rawChecksum;
    uint64_t reserved;
};

struct BlockRecord {
    uint64_t index;
    uint64_t rawSize;
    uint64_t compressedSize;
    uint64_t rawChecksum;
};

static_assert(sizeof(FileHeader) == 56, "unexpected file header size");
static_assert(sizeof(BlockRecord) == 32, "unexpected block record size");

struct WorkerSlot {
    uint64_t blockStart;
    uint64_t blockEnd;
    uint64_t input;
    uint64_t inputBytes;
    uint64_t output;
    uint64_t outputCapacity;
    uint64_t outputBytes;
    uint64_t rawBytes;
    uint64_t elapsedMicros;
    uint64_t pageFaults;
    uint64_t pagesSent;
    uint64_t pagesReceived;
    uint64_t invalidations;
    int32_t error;
    uint32_t padding;
};

struct Descriptor {
    uint64_t magic;
    uint32_t version;
    uint32_t mode;
    uint32_t workerCount;
    int32_t barrierId;
    int32_t mutexId;
    uint32_t joinedWorkers;
    uint64_t blockSize;
    uint64_t rawSize;
    uint64_t blockCount;
    uint64_t rawChecksum;
    uint32_t codec;
    uint32_t abort;
    WorkerSlot workers[kMaximumWorkers];
};

static_assert(sizeof(Descriptor) <= 4096, "descriptor must fit in one page");

struct ParsedBlock {
    BlockRecord record;
    uint64_t payloadOffset;
};

struct Options {
    std::string configPath = kDefaultConfigPath;
    int nodeId = -1;
    uint32_t workers = 1;
    uint64_t blockSize = kDefaultBlockSize;
    uint64_t joinAddress = 0;
    uint32_t mode = 0;
    std::string inputPath;
    std::string outputPath;
    bool creatorOptionGiven = false;
    bool help = false;
};

using Clock = std::chrono::steady_clock;

void printUsage() {
    std::printf(
        "Usage:\n"
        "  compress [--config <path>] [--node <id>] [--workers <n>] [--block-size <bytes>] "
        "compress <input> <output>\n"
        "  compress [--config <path>] [--node <id>] [--workers <n>] [--block-size <bytes>] "
        "decompress <input> <output>\n"
        "  compress --join <descriptor_address> [--config <path>] [--node <id>]\n"
        "\n"
        "The process started without --join reads the input, becomes worker 0 and prints the join\n"
        "command. Run that command once per remaining worker, on any machine of the cluster.\n"
        "--node picks the local DSM node only when several run on the same machine.\n"
        "The compressed format is a header followed by independently compressed LZSS blocks.\n"
        "Example:\n"
        "  compress --workers 3 compress input.bin output.dsmc\n"
        "  compress --join 0x100000000000\n"
        "  compress --workers 3 decompress output.dsmc restored.bin\n");
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

bool parseOptions(int argumentCount, char** arguments, Options& options) {
    std::vector<std::string> positional;
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
            options.creatorOptionGiven = true;
        } else if (argument == "--block-size") {
            const char* text = valueFor("--block-size");
            if (text == nullptr || !parseU64(text, options.blockSize) || options.blockSize == 0) {
                std::fprintf(stderr, "invalid --block-size value\n");
                return false;
            }
            options.creatorOptionGiven = true;
        } else if (argument == "--join") {
            const char* text = valueFor("--join");
            if (text == nullptr || !parseU64(text, options.joinAddress) || options.joinAddress == 0) {
                std::fprintf(stderr, "invalid --join value\n");
                return false;
            }
        } else if (argument.size() > 1 && argument[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", argument.c_str());
            return false;
        } else {
            positional.push_back(argument);
        }
    }

    if (options.help) {
        return true;
    }
    if (options.joinAddress != 0) {
        if (!positional.empty()) {
            std::fprintf(stderr, "--join does not take a command or file names\n");
            return false;
        }
        if (options.creatorOptionGiven) {
            std::fprintf(stderr, "--workers and --block-size only apply to the process that creates the run\n");
            return false;
        }
        return true;
    }

    if (positional.size() != 3) {
        std::fprintf(stderr, "expected a command, an input file and an output file\n");
        return false;
    }
    if (positional[0] == "compress") {
        options.mode = kModeCompress;
    } else if (positional[0] == "decompress") {
        options.mode = kModeDecompress;
    } else {
        std::fprintf(stderr, "unknown command %s\n", positional[0].c_str());
        return false;
    }
    options.inputPath = positional[1];
    options.outputPath = positional[2];
    return true;
}

uint8_t* bufferFromAddress(uint64_t address) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
}

const uint8_t* readBufferFromAddress(uint64_t address) {
    return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address));
}

uint64_t addressOf(const void* pointer) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
}

uint64_t rawSizeOfBlock(uint64_t rawSize, uint64_t blockSize, uint64_t blockIndex) {
    const uint64_t offset = blockIndex * blockSize;
    if (offset >= rawSize) {
        return 0;
    }
    return std::min(blockSize, rawSize - offset);
}

bool readFile(const std::string& path, std::vector<uint8_t>& outData) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) {
        std::fprintf(stderr, "cannot determine the size of %s\n", path.c_str());
        return false;
    }
    input.seekg(0, std::ios::beg);
    outData.resize(static_cast<size_t>(length));
    if (length > 0) {
        input.read(reinterpret_cast<char*>(outData.data()), length);
        if (input.gcount() != length) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
    }
    return true;
}

bool writeData(std::ofstream& output, const void* data, size_t size) {
    if (size == 0) {
        return true;
    }
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
}

FileHeader makeFileHeader(const std::vector<uint8_t>& file, uint64_t blockSize) {
    FileHeader header{};
    std::memcpy(header.magic, kFileMagic, sizeof(header.magic));
    header.version = static_cast<uint32_t>(kFileVersion);
    header.codec = compressor::kCodecLzss;
    header.rawSize = file.size();
    header.blockSize = blockSize;
    header.blockCount = file.empty() ? 0 : (static_cast<uint64_t>(file.size()) - 1) / blockSize + 1;
    header.rawChecksum = compressor::checksum(file.data(), file.size());
    return header;
}

bool parseCompressedFile(const std::vector<uint8_t>& file, FileHeader& outHeader,
                         std::vector<ParsedBlock>& outBlocks) {
    if (file.size() < sizeof(FileHeader)) {
        std::fprintf(stderr, "input is too short to be a compressed file\n");
        return false;
    }
    FileHeader header{};
    std::memcpy(&header, file.data(), sizeof(header));
    if (std::memcmp(header.magic, kFileMagic, sizeof(header.magic)) != 0) {
        std::fprintf(stderr, "input is not a DSM compressed file\n");
        return false;
    }
    if (header.version != kFileVersion) {
        std::fprintf(stderr, "unsupported compressed file version %u\n", header.version);
        return false;
    }
    if (header.codec != compressor::kCodecLzss) {
        std::fprintf(stderr, "unsupported codec %u\n", header.codec);
        return false;
    }
    if (header.blockSize == 0) {
        std::fprintf(stderr, "compressed file has a zero block size\n");
        return false;
    }
    const uint64_t expectedBlocks =
        header.rawSize == 0 ? 0 : (header.rawSize - 1) / header.blockSize + 1;
    if (header.blockCount != expectedBlocks) {
        std::fprintf(stderr, "compressed file has an inconsistent block count\n");
        return false;
    }
    const uint64_t maximumRecords = (file.size() - sizeof(FileHeader)) / sizeof(BlockRecord);
    if (header.blockCount > maximumRecords) {
        std::fprintf(stderr, "compressed file has too many blocks\n");
        return false;
    }

    outBlocks.resize(static_cast<size_t>(header.blockCount));
    uint64_t offset = sizeof(FileHeader);
    for (uint64_t index = 0; index < header.blockCount; ++index) {
        if (offset + sizeof(BlockRecord) > file.size()) {
            std::fprintf(stderr, "compressed file is truncated at block %llu\n",
                         static_cast<unsigned long long>(index));
            return false;
        }
        BlockRecord record{};
        std::memcpy(&record, file.data() + offset, sizeof(record));
        if (record.index != index) {
            std::fprintf(stderr, "compressed file has blocks out of order\n");
            return false;
        }
        const uint64_t expectedRaw = rawSizeOfBlock(header.rawSize, header.blockSize, index);
        if (record.rawSize != expectedRaw) {
            std::fprintf(stderr, "compressed file has an invalid raw size at block %llu\n",
                         static_cast<unsigned long long>(index));
            return false;
        }
        if (record.compressedSize > compressor::worstCaseSize(record.rawSize)) {
            std::fprintf(stderr, "compressed file has an invalid compressed size at block %llu\n",
                         static_cast<unsigned long long>(index));
            return false;
        }
        offset += sizeof(BlockRecord);
        if (offset + record.compressedSize > file.size()) {
            std::fprintf(stderr, "compressed file is truncated at block %llu\n",
                         static_cast<unsigned long long>(index));
            return false;
        }
        outBlocks[static_cast<size_t>(index)] = ParsedBlock{record, offset};
        offset += record.compressedSize;
    }
    if (offset != file.size()) {
        std::fprintf(stderr, "compressed file has trailing data\n");
        return false;
    }
    outHeader = header;
    return true;
}

void assignBlocks(Descriptor* descriptor) {
    const uint64_t base = descriptor->blockCount / descriptor->workerCount;
    const uint64_t remainder = descriptor->blockCount % descriptor->workerCount;
    uint64_t nextBlock = 0;
    for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
        const uint64_t count = base + (worker < remainder ? 1 : 0);
        descriptor->workers[worker].blockStart = nextBlock;
        descriptor->workers[worker].blockEnd = nextBlock + count;
        nextBlock += count;
    }
}

void prepareSlots(Descriptor* descriptor, const std::vector<ParsedBlock>& parsed) {
    for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
        WorkerSlot& slot = descriptor->workers[worker];
        uint64_t rawBytes = 0;
        uint64_t inputBytes = 0;
        uint64_t outputCapacity = 0;
        for (uint64_t block = slot.blockStart; block < slot.blockEnd; ++block) {
            const uint64_t rawSize = rawSizeOfBlock(descriptor->rawSize, descriptor->blockSize, block);
            rawBytes += rawSize;
            if (descriptor->mode == kModeCompress) {
                inputBytes += rawSize;
                outputCapacity += compressor::worstCaseSize(rawSize) + sizeof(BlockRecord);
            } else {
                const BlockRecord& record = parsed[static_cast<size_t>(block)].record;
                inputBytes += sizeof(BlockRecord) + record.compressedSize;
                outputCapacity += rawSize;
            }
        }
        slot.rawBytes = rawBytes;
        slot.inputBytes = inputBytes;
        slot.outputCapacity = outputCapacity;
    }
}

bool syncBarrier(int barrierId) {
    const int status = dsm_barrier(barrierId);
    if (status < 0) {
        std::fprintf(stderr, "dsm_barrier failed with status %d\n", status);
        return false;
    }
    return true;
}

Descriptor* createDescriptor(const FileHeader& header, const Options& options,
                             const std::vector<ParsedBlock>& parsed) {
    Descriptor* descriptor = static_cast<Descriptor*>(dsm_malloc(sizeof(Descriptor)));
    if (descriptor == nullptr) {
        std::fprintf(stderr, "dsm_malloc failed: %s\n", std::strerror(errno));
        return nullptr;
    }
    std::memset(descriptor, 0, sizeof(Descriptor));
    descriptor->magic = kDescriptorMagic;
    descriptor->version = kDescriptorVersion;
    descriptor->mode = options.mode;
    descriptor->workerCount = options.workers;
    descriptor->barrierId = -1;
    descriptor->mutexId = -1;
    descriptor->joinedWorkers = 1;
    descriptor->blockSize = header.blockSize;
    descriptor->rawSize = header.rawSize;
    descriptor->blockCount = header.blockCount;
    descriptor->rawChecksum = header.rawChecksum;
    descriptor->codec = header.codec;
    assignBlocks(descriptor);
    prepareSlots(descriptor, parsed);
    descriptor->barrierId = dsm_barrier_init(static_cast<int>(descriptor->workerCount));
    if (descriptor->barrierId < 0) {
        std::fprintf(stderr, "dsm_barrier_init failed with status %d\n", descriptor->barrierId);
        dsm_free(descriptor);
        return nullptr;
    }
    descriptor->mutexId = dsm_mutex_init();
    if (descriptor->mutexId < 0) {
        std::fprintf(stderr, "dsm_mutex_init failed with status %d\n", descriptor->mutexId);
        dsm_free(descriptor);
        return nullptr;
    }
    return descriptor;
}

void printStart(const Descriptor& descriptor, const Options& options) {
    if (descriptor.mode == kModeCompress) {
        std::printf("Compress %llu bytes with %llu-byte blocks on %u worker(s)\n",
                    static_cast<unsigned long long>(descriptor.rawSize),
                    static_cast<unsigned long long>(descriptor.blockSize),
                    static_cast<unsigned>(descriptor.workerCount));
    } else {
        std::printf("Decompress %llu bytes from %llu block(s) on %u worker(s)\n",
                    static_cast<unsigned long long>(descriptor.rawSize),
                    static_cast<unsigned long long>(descriptor.blockCount),
                    static_cast<unsigned>(descriptor.workerCount));
    }
    if (descriptor.workerCount > 1) {
        std::printf("Run this command %u more time(s), on any machine of the cluster:\n",
                    static_cast<unsigned>(descriptor.workerCount - 1));
        std::printf("  compress --join 0x%llx",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&descriptor)));
        if (options.configPath != kDefaultConfigPath) {
            std::printf(" --config %s", options.configPath.c_str());
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

bool allocateSlot(WorkerSlot& slot) {
    if (slot.inputBytes > 0) {
        void* input = dsm_malloc(static_cast<size_t>(slot.inputBytes));
        if (input == nullptr) {
            std::fprintf(stderr, "dsm_malloc(%llu) for input failed: %s\n",
                         static_cast<unsigned long long>(slot.inputBytes), std::strerror(errno));
            return false;
        }
        slot.input = addressOf(input);
    }
    if (slot.outputCapacity > 0) {
        void* output = dsm_malloc(static_cast<size_t>(slot.outputCapacity));
        if (output == nullptr) {
            std::fprintf(stderr, "dsm_malloc(%llu) for output failed: %s\n",
                         static_cast<unsigned long long>(slot.outputCapacity), std::strerror(errno));
            return false;
        }
        slot.output = addressOf(output);
    }
    return true;
}

void freeAddresses(uint64_t input, uint64_t output) {
    if (input != 0) {
        dsm_free(bufferFromAddress(input));
    }
    if (output != 0) {
        dsm_free(bufferFromAddress(output));
    }
}

bool distributeInput(Descriptor* descriptor, const std::vector<uint8_t>& file,
                     const std::vector<ParsedBlock>& parsed) {
    for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
        const WorkerSlot& slot = descriptor->workers[worker];
        uint8_t* destination = bufferFromAddress(slot.input);
        if (descriptor->mode == kModeCompress) {
            const uint64_t offset = slot.blockStart * descriptor->blockSize;
            if (offset + slot.inputBytes > file.size()) {
                std::fprintf(stderr, "input range for worker %u is out of bounds\n", worker);
                return false;
            }
            if (slot.inputBytes > 0) {
                std::memcpy(destination, file.data() + offset, static_cast<size_t>(slot.inputBytes));
            }
            continue;
        }

        uint64_t cursor = 0;
        for (uint64_t block = slot.blockStart; block < slot.blockEnd; ++block) {
            const ParsedBlock& parsedBlock = parsed[static_cast<size_t>(block)];
            const uint64_t recordSize = sizeof(BlockRecord) + parsedBlock.record.compressedSize;
            if (cursor + recordSize > slot.inputBytes) {
                std::fprintf(stderr, "input range for worker %u is out of bounds\n", worker);
                return false;
            }
            std::memcpy(destination + cursor, &parsedBlock.record, sizeof(BlockRecord));
            cursor += sizeof(BlockRecord);
            if (parsedBlock.record.compressedSize > 0) {
                std::memcpy(destination + cursor, file.data() + parsedBlock.payloadOffset,
                            static_cast<size_t>(parsedBlock.record.compressedSize));
                cursor += parsedBlock.record.compressedSize;
            }
        }
        if (cursor != slot.inputBytes) {
            std::fprintf(stderr, "input range for worker %u is inconsistent\n", worker);
            return false;
        }
    }
    return true;
}

int compressSlot(const Descriptor& descriptor, WorkerSlot& slot) {
    const uint8_t* input = readBufferFromAddress(slot.input);
    uint8_t* output = bufferFromAddress(slot.output);
    uint64_t inputOffset = 0;
    uint64_t outputOffset = 0;
    std::vector<uint8_t> compressed;

    for (uint64_t block = slot.blockStart; block < slot.blockEnd; ++block) {
        const uint64_t rawSize = rawSizeOfBlock(descriptor.rawSize, descriptor.blockSize, block);
        compressed.clear();
        if (!compressor::encode(input + inputOffset, static_cast<size_t>(rawSize), compressed)) {
            return DSM_ERR_INTERNAL;
        }
        if (outputOffset + sizeof(BlockRecord) + compressed.size() > slot.outputCapacity) {
            return DSM_ERR_NO_MEMORY;
        }
        const BlockRecord record{block, rawSize, compressed.size(),
                                 compressor::checksum(input + inputOffset, static_cast<size_t>(rawSize))};
        std::memcpy(output + outputOffset, &record, sizeof(record));
        outputOffset += sizeof(record);
        if (!compressed.empty()) {
            std::memcpy(output + outputOffset, compressed.data(), compressed.size());
            outputOffset += compressed.size();
        }
        inputOffset += rawSize;
    }
    if (inputOffset != slot.inputBytes) {
        return DSM_ERR_SIZE_MISMATCH;
    }
    slot.outputBytes = outputOffset;
    return DSM_OK;
}

int decompressSlot(const Descriptor& descriptor, WorkerSlot& slot) {
    const uint8_t* input = readBufferFromAddress(slot.input);
    uint8_t* output = bufferFromAddress(slot.output);
    uint64_t inputOffset = 0;
    uint64_t outputOffset = 0;

    for (uint64_t block = slot.blockStart; block < slot.blockEnd; ++block) {
        if (inputOffset + sizeof(BlockRecord) > slot.inputBytes) {
            return DSM_ERR_SIZE_MISMATCH;
        }
        BlockRecord record{};
        std::memcpy(&record, input + inputOffset, sizeof(record));
        inputOffset += sizeof(record);
        if (record.index != block) {
            return DSM_ERR_INVALID_MESSAGE;
        }
        const uint64_t expectedRaw = rawSizeOfBlock(descriptor.rawSize, descriptor.blockSize, block);
        if (record.rawSize != expectedRaw || record.compressedSize > slot.inputBytes - inputOffset) {
            return DSM_ERR_SIZE_MISMATCH;
        }
        if (outputOffset + record.rawSize > slot.outputCapacity) {
            return DSM_ERR_NO_MEMORY;
        }
        size_t produced = 0;
        if (!compressor::decode(input + inputOffset, static_cast<size_t>(record.compressedSize),
                                output + outputOffset, static_cast<size_t>(record.rawSize), produced)) {
            return DSM_ERR_INVALID_MESSAGE;
        }
        if (produced != record.rawSize ||
            compressor::checksum(output + outputOffset, produced) != record.rawChecksum) {
            return DSM_ERR_INVALID_MESSAGE;
        }
        inputOffset += record.compressedSize;
        outputOffset += record.rawSize;
    }
    if (inputOffset != slot.inputBytes) {
        return DSM_ERR_SIZE_MISMATCH;
    }
    slot.outputBytes = outputOffset;
    return DSM_OK;
}

void recordStats(WorkerSlot& slot, Clock::time_point start, const DsmStats& statsBefore) {
    const apps::RunStats stats = apps::statsSince(statsBefore);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    slot.elapsedMicros = static_cast<uint64_t>(elapsed.count());
    slot.pageFaults = stats.pageFaults;
    slot.pagesSent = stats.pagesSent;
    slot.pagesReceived = stats.pagesReceived;
    slot.invalidations = stats.invalidations;
}

bool writeCompressedOutput(const Descriptor& descriptor, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot create %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    FileHeader header{};
    std::memcpy(header.magic, kFileMagic, sizeof(header.magic));
    header.version = static_cast<uint32_t>(kFileVersion);
    header.codec = descriptor.codec;
    header.rawSize = descriptor.rawSize;
    header.blockSize = descriptor.blockSize;
    header.blockCount = descriptor.blockCount;
    header.rawChecksum = descriptor.rawChecksum;
    if (!writeData(output, &header, sizeof(header))) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }

    std::vector<uint8_t> staging;
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const WorkerSlot& slot = descriptor.workers[worker];
        const uint8_t* buffer = readBufferFromAddress(slot.output);
        uint64_t cursor = 0;
        for (uint64_t block = slot.blockStart; block < slot.blockEnd; ++block) {
            if (cursor + sizeof(BlockRecord) > slot.outputBytes) {
                std::fprintf(stderr, "worker %u produced an inconsistent output\n", worker);
                return false;
            }
            BlockRecord record{};
            std::memcpy(&record, buffer + cursor, sizeof(record));
            if (record.index != block ||
                record.rawSize != rawSizeOfBlock(descriptor.rawSize, descriptor.blockSize, block) ||
                record.compressedSize > compressor::worstCaseSize(record.rawSize) ||
                cursor + sizeof(BlockRecord) + record.compressedSize > slot.outputBytes) {
                std::fprintf(stderr, "worker %u produced an invalid block %llu\n", worker,
                             static_cast<unsigned long long>(block));
                return false;
            }
            if (!writeData(output, &record, sizeof(record))) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return false;
            }
            cursor += sizeof(BlockRecord);
            staging.resize(static_cast<size_t>(record.compressedSize));
            if (!staging.empty()) {
                std::memcpy(staging.data(), buffer + cursor, staging.size());
            }
            if (!writeData(output, staging.data(), staging.size())) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return false;
            }
            cursor += record.compressedSize;
        }
        if (cursor != slot.outputBytes) {
            std::fprintf(stderr, "worker %u produced trailing output\n", worker);
            return false;
        }
    }
    return static_cast<bool>(output);
}

bool writeDecompressedOutput(const Descriptor& descriptor, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot create %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    constexpr uint64_t kCopyChunk = 1 << 20;
    std::vector<uint8_t> staging;
    uint64_t written = 0;
    uint64_t checksum = compressor::checksum(nullptr, 0);
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const WorkerSlot& slot = descriptor.workers[worker];
        if (slot.outputBytes != slot.rawBytes) {
            std::fprintf(stderr, "worker %u produced an inconsistent output\n", worker);
            return false;
        }
        const uint8_t* buffer = readBufferFromAddress(slot.output);
        uint64_t offset = 0;
        while (offset < slot.outputBytes) {
            const uint64_t chunk = std::min(kCopyChunk, slot.outputBytes - offset);
            staging.resize(static_cast<size_t>(chunk));
            std::memcpy(staging.data(), buffer + offset, staging.size());
            if (!writeData(output, staging.data(), staging.size())) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return false;
            }
            checksum = compressor::checksumExtend(checksum, staging.data(), staging.size());
            offset += chunk;
        }
        written += slot.outputBytes;
    }
    if (written != descriptor.rawSize) {
        std::fprintf(stderr, "decompressed %llu bytes, expected %llu\n",
                     static_cast<unsigned long long>(written),
                     static_cast<unsigned long long>(descriptor.rawSize));
        return false;
    }
    if (checksum != descriptor.rawChecksum) {
        std::fprintf(stderr, "decompressed checksum %016llx does not match %016llx\n",
                     static_cast<unsigned long long>(checksum),
                     static_cast<unsigned long long>(descriptor.rawChecksum));
        return false;
    }
    return static_cast<bool>(output);
}

void printSummary(const Descriptor& descriptor) {
    uint64_t totalBlocks = 0;
    uint64_t totalOutput = 0;
    uint64_t maximumElapsed = 0;
    if (descriptor.mode == kModeCompress) {
        std::printf("Compression finished: %llu -> ", static_cast<unsigned long long>(descriptor.rawSize));
    } else {
        std::printf("Decompression finished: %llu bytes, ", static_cast<unsigned long long>(descriptor.rawSize));
    }
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        totalBlocks += descriptor.workers[worker].blockEnd - descriptor.workers[worker].blockStart;
        totalOutput += descriptor.workers[worker].outputBytes;
        maximumElapsed = std::max(maximumElapsed, descriptor.workers[worker].elapsedMicros);
    }
    if (descriptor.mode == kModeCompress) {
        std::printf("%llu bytes in %llu block(s)\n", static_cast<unsigned long long>(totalOutput),
                    static_cast<unsigned long long>(totalBlocks));
    } else {
        std::printf("%llu block(s)\n", static_cast<unsigned long long>(totalBlocks));
    }
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const WorkerSlot& slot = descriptor.workers[worker];
        std::printf(
            "  worker %u blocks [%llu, %llu) raw %llu bytes, out %llu bytes, %.3f s, "
            "faults %llu sent %llu recv %llu invalidations %llu\n",
            static_cast<unsigned>(worker), static_cast<unsigned long long>(slot.blockStart),
            static_cast<unsigned long long>(slot.blockEnd),
            static_cast<unsigned long long>(slot.rawBytes),
            static_cast<unsigned long long>(slot.outputBytes),
            static_cast<double>(slot.elapsedMicros) / 1000000.0,
            static_cast<unsigned long long>(slot.pageFaults),
            static_cast<unsigned long long>(slot.pagesSent),
            static_cast<unsigned long long>(slot.pagesReceived),
            static_cast<unsigned long long>(slot.invalidations));
    }
    if (descriptor.mode == kModeCompress && descriptor.rawSize > 0) {
        const double ratio = static_cast<double>(totalOutput) * 100.0 / static_cast<double>(descriptor.rawSize);
        std::printf("  ratio %.2f%%, slowest worker %.3f s\n", ratio,
                    static_cast<double>(maximumElapsed) / 1000000.0);
    }
    std::fflush(stdout);
}

const char* describeWorkerError(int32_t status) {
    switch (status) {
        case DSM_ERR_NO_MEMORY: return "not enough DSM memory for its buffers";
        case DSM_ERR_INVALID_MESSAGE: return "a compressed block is corrupt";
        case DSM_ERR_SIZE_MISMATCH: return "its blocks do not match the sizes in the file header";
        case DSM_ERR_INTERNAL: return "a block grew beyond its worst-case compressed size";
        default: return "unexpected error";
    }
}

void printErrors(const Descriptor& descriptor) {
    for (uint32_t worker = 0; worker < descriptor.workerCount; ++worker) {
        const int32_t error = descriptor.workers[worker].error;
        if (error != 0) {
            std::fprintf(stderr, "worker %u failed: %s (status %d)\n", worker, describeWorkerError(error), error);
        }
    }
}

int runSession(Descriptor* descriptor, int workerIndex, bool creator, const std::vector<uint8_t>* file,
               const std::vector<ParsedBlock>* parsed, const std::string& outputPath) {
    WorkerSlot& slot = descriptor->workers[workerIndex];
    if (!allocateSlot(slot)) {
        slot.error = DSM_ERR_NO_MEMORY;
    }
    const uint64_t inputAddress = slot.input;
    const uint64_t outputAddress = slot.output;
    if (!syncBarrier(descriptor->barrierId)) {
        freeAddresses(inputAddress, outputAddress);
        if (creator) {
            dsm_free(descriptor);
        }
        return 1;
    }

    if (creator) {
        dsm_mutex_destroy(descriptor->mutexId);
        bool anyError = false;
        for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
            if (descriptor->workers[worker].error != 0) {
                anyError = true;
            }
        }
        if (anyError) {
            descriptor->abort = 1;
        } else if (file == nullptr || parsed == nullptr ||
                   !distributeInput(descriptor, *file, *parsed)) {
            descriptor->abort = 1;
        }
    }
    if (!syncBarrier(descriptor->barrierId)) {
        freeAddresses(inputAddress, outputAddress);
        if (creator) {
            dsm_free(descriptor);
        }
        return 1;
    }

    if (descriptor->abort == 0 && slot.error == 0) {
        const DsmStats statsBefore = apps::readNodeStats();
        const Clock::time_point start = Clock::now();
        const int status = descriptor->mode == kModeCompress ? compressSlot(*descriptor, slot)
                                                             : decompressSlot(*descriptor, slot);
        recordStats(slot, start, statsBefore);
        if (status != DSM_OK) {
            slot.error = status;
        }
    }
    if (!syncBarrier(descriptor->barrierId)) {
        freeAddresses(inputAddress, outputAddress);
        if (creator) {
            dsm_free(descriptor);
        }
        return 1;
    }

    int result = 0;
    if (creator) {
        bool anyError = descriptor->abort != 0;
        for (uint32_t worker = 0; worker < descriptor->workerCount; ++worker) {
            if (descriptor->workers[worker].error != 0) {
                anyError = true;
            }
        }
        if (anyError) {
            descriptor->abort = 1;
            printErrors(*descriptor);
            result = 1;
        } else {
            const bool written = descriptor->mode == kModeCompress
                                     ? writeCompressedOutput(*descriptor, outputPath)
                                     : writeDecompressedOutput(*descriptor, outputPath);
            if (!written) {
                descriptor->abort = 1;
                result = 1;
            } else {
                printSummary(*descriptor);
            }
        }
    }
    if (!syncBarrier(descriptor->barrierId)) {
        freeAddresses(inputAddress, outputAddress);
        if (creator) {
            dsm_free(descriptor);
        }
        return 1;
    }

    const bool failed = descriptor->abort != 0 || slot.error != 0;
    if (!syncBarrier(descriptor->barrierId)) {
        freeAddresses(inputAddress, outputAddress);
        if (creator) {
            dsm_free(descriptor);
        }
        return 1;
    }

    freeAddresses(inputAddress, outputAddress);
    if (creator) {
        dsm_free(descriptor);
        return result;
    }
    return failed ? 1 : 0;
}

int runCreator(const Options& options) {
    std::vector<uint8_t> file;
    if (!readFile(options.inputPath, file)) {
        return 1;
    }
    FileHeader header{};
    std::vector<ParsedBlock> parsed;
    if (options.mode == kModeCompress) {
        header = makeFileHeader(file, options.blockSize);
    } else if (!parseCompressedFile(file, header, parsed)) {
        return 1;
    }

    Descriptor* descriptor = createDescriptor(header, options, parsed);
    if (descriptor == nullptr) {
        return 1;
    }
    printStart(*descriptor, options);
    return runSession(descriptor, 0, true, &file, &parsed, options.outputPath);
}

int runJoiner(const Options& options) {
    Descriptor* descriptor = reinterpret_cast<Descriptor*>(static_cast<uintptr_t>(options.joinAddress));
    if (descriptor->magic != kDescriptorMagic || descriptor->version != kDescriptorVersion) {
        std::fprintf(stderr, "address 0x%llx is not a compress descriptor\n",
                     static_cast<unsigned long long>(options.joinAddress));
        return 1;
    }
    if (descriptor->mode != kModeCompress && descriptor->mode != kModeDecompress) {
        std::fprintf(stderr, "descriptor has an invalid mode\n");
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
    std::printf("Joined as worker %u of %u\n", static_cast<unsigned>(workerIndex),
                static_cast<unsigned>(descriptor->workerCount));
    std::fflush(stdout);
    return runSession(descriptor, static_cast<int>(workerIndex), false, nullptr, nullptr, std::string());
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
    const int status = dsm_init(options.configPath.c_str(), options.nodeId, 0);
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
