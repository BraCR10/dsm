#include "memory/shared_window.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/log.h"

namespace dsm::memory {

namespace {

constexpr const char* kModule = "memory";

}  // namespace

std::string nodeSharedMemoryName(uint32_t nodeId) {
    return "/dsm-node-" + std::to_string(nodeId);
}

SharedWindow::~SharedWindow() {
    close();
}

DsmStatus SharedWindow::createForNode(const std::string& name, uint64_t size) {
    shm_unlink(name.c_str());
    descriptor_ = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor_ < 0 || ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
        common::logError(kModule, common::fromSelf(),
                         "cannot create shared memory %s: %s", name.c_str(), std::strerror(errno));
        close();
        return DSM_ERR_NO_MEMORY;
    }
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, descriptor_, 0);
    if (mapping == MAP_FAILED) {
        common::logError(kModule, common::fromSelf(),
                         "cannot map shared memory %s: %s", name.c_str(), std::strerror(errno));
        close();
        return DSM_ERR_NO_MEMORY;
    }
    name_ = name;
    data_ = static_cast<uint8_t*>(mapping);
    size_ = size;
    ownsObject_ = true;
    return DSM_OK;
}

DsmStatus SharedWindow::openForApplication(const std::string& name, uint64_t size, uint64_t baseAddress) {
    descriptor_ = shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    if (descriptor_ < 0) {
        return DSM_ERR_NODE_UNAVAILABLE;
    }
    void* requested = reinterpret_cast<void*>(baseAddress);
    void* mapping = mmap(requested, size, PROT_NONE, MAP_SHARED | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                         descriptor_, 0);
    if (mapping == MAP_FAILED || mapping != requested) {
        if (mapping != MAP_FAILED) {
            munmap(mapping, size);
        }
        close();
        return DSM_ERR_NO_MEMORY;
    }
    name_ = name;
    data_ = static_cast<uint8_t*>(mapping);
    size_ = size;
    return DSM_OK;
}

void SharedWindow::discardPage(uint64_t offset, uint64_t pageSize) {
    fallocate(descriptor_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
              static_cast<off_t>(pageSize));
}

void SharedWindow::close() {
    if (data_ != nullptr) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    if (ownsObject_) {
        shm_unlink(name_.c_str());
        ownsObject_ = false;
    }
}

}  // namespace dsm::memory
