#ifndef HK_CELLULAR_SELECTION_H
#define HK_CELLULAR_SELECTION_H

#include <stdint.h>

#define HK_CELLULAR_SELECTION_NAME "kern.hackios.cellular_selection"
#define HK_CELLULAR_SELECTION_VERSION 1u

enum hk_selection_status {
    HK_SELECTION_UNKNOWN,
    HK_SELECTION_COMPLETE,
    HK_SELECTION_INDICATION_ERROR
};

/* checked_ms is the owner's receive-window evaluation time. No profile
 * identifier, carrier name or subscription identifier crosses this ABI. */
struct hk_selection_report {
    uint64_t checked_ms;
    uint32_t status;
    uint32_t indication_error;
    uint32_t active_present;
    uint32_t pending_present;
};

struct hk_cellular_selection {
    uint32_t version;
    uint32_t size;
    uint32_t phase;
    uint32_t reserved;
    uint64_t now_ms;
    struct hk_selection_report platform;
    struct hk_selection_report software_sub0;
};

#endif
