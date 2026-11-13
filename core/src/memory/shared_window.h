#ifndef DSM_MEMORY_SHARED_WINDOW_H
#define DSM_MEMORY_SHARED_WINDOW_H

#include <cstdint>
#include <string>

#include "dsm/dsm.h"

namespace dsm::memory {

std::string nodeSharedMemoryName(uint32_t nodeId);

// POSIX shared memory object (Silberschatz 3.5.1) that holds the page data of one node.
// The node maps it read-write anywhere; each application maps it at the common base address
// with PROT_NONE and opens pages with mprotect as the node grants access.
class SharedWindow {
public:
    SharedWindow() = default;
    ~SharedWindow();
    SharedWindow(const SharedWindow&) = delete;
    SharedWindow& operator=(const SharedWindow&) = delete;

    DsmStatus createForNode(const std::string& name, uint64_t size);
    DsmStatus openForApplication(const std::string& name, uint64_t size, uint64_t baseAddress);
    void close();

    uint8_t* data() const { return data_; }
    void discardPage(uint64_t offset, uint64_t pageSize);

private:
    std::string name_;
    int descriptor_ = -1;
    uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
    bool ownsObject_ = false;
};

}  // namespace dsm::memory

#endif
