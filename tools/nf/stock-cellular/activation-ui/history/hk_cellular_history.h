#ifndef HK_CELLULAR_HISTORY_H
#define HK_CELLULAR_HISTORY_H
#include "hk_cellular_observation.h"

#define HK_CELLULAR_HISTORY_NAME "kern.hackios.cellular_history"
#define HK_CELLULAR_HISTORY_VERSION 1u

/* This boot's in-memory observations only. last.valid_mask is historical;
 * only the existing cellular_observation sysctl can identify live values.
 * Each group keeps its own source timestamp, so these are not represented
 * as simultaneous measurements. now_ms is read from the same boot clock. */
struct hk_cellular_history {
    uint32_t version,size,phase,retained;
    uint64_t now_ms,latest_field_ms,stopped_ms,errfatal_ms;
    uint32_t online_attempts,online_submitted,online_generation,online_tid;
    uint32_t online_ack_seen,online_result,online_error,reserved;
    uint64_t online_attempt_ms,online_ack_ms;
    struct hk_cellular_observation last;
};
#endif
