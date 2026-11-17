#ifndef LX_AWDL_ROC_DEADLINE_H
#define LX_AWDL_ROC_DEADLINE_H
#include <stdint.h>
/* A late grant cannot recover a rendezvous that already ended. An unplanned
 * social visit gets at most 200 ms to start. This is only a cancellation
 * deadline, never authorization to transmit. Grant/event checks still apply. */
static inline uint64_t awdl_latest_grant(uint64_t now,uint64_t planned_end)
{
    uint64_t cap=UINT64_MAX-now<200000?UINT64_MAX:now+200000;
    if(planned_end>now && planned_end-now>4096 && planned_end-4096<cap)
        return planned_end-4096;
    return cap;
}
#endif
