/* A legacy unicast mDNS answer may live only ten seconds. An idle data
 * reservation must not consume that entire lifetime for another peer's
 * pending native discovery packet. Busy unicast queues retain priority in
 * the caller; never interrupt a live firmware flight here. */
#ifndef HK_AWDL_GROUP_DEADLINE_H
#define HK_AWDL_GROUP_DEADLINE_H
#include <stdint.h>
#include <stdbool.h>
static inline bool hk_awdl_group_yield(uint64_t now,uint64_t queued,
    bool mdns,bool in_flight)
{
    return mdns && !in_flight && now>=queued && now-queued>=2000000ull;
}
#endif
