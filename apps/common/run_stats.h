#ifndef DSM_APPS_COMMON_RUN_STATS_H
#define DSM_APPS_COMMON_RUN_STATS_H

#include "dsm/dsm.h"

namespace apps {

struct RunStats {
    uint64_t pageFaults = 0;
    uint64_t pagesSent = 0;
    uint64_t pagesReceived = 0;
    uint64_t invalidations = 0;
};

// The counters of a node grow from the moment it starts, so a run reports the difference between
// a reading taken before its work and one taken after. Applications sharing a node share its counters.
inline DsmStats readNodeStats() {
    DsmStats stats{};
    if (dsm_get_stats(&stats) != DSM_OK) {
        stats = DsmStats{};
    }
    return stats;
}

inline RunStats statsSince(const DsmStats& before) {
    DsmStats after{};
    RunStats run;
    if (dsm_get_stats(&after) != DSM_OK || after.page_faults < before.page_faults) {
        return run;
    }
    run.pageFaults = after.page_faults - before.page_faults;
    run.pagesSent = after.pages_sent - before.pages_sent;
    run.pagesReceived = after.pages_received - before.pages_received;
    run.invalidations = after.invalidations - before.invalidations;
    return run;
}

}

#endif
