#ifndef DSM_MEMORY_PAGE_ACCESS_CONTROLLER_H
#define DSM_MEMORY_PAGE_ACCESS_CONTROLLER_H

#include <cstdint>

namespace dsm::memory {

class PageAccessController {
public:
    virtual ~PageAccessController() = default;
    virtual void restrictApplicationAccess(uint64_t pageId, int protection) = 0;
};

}  // namespace dsm::memory

#endif
