#ifndef HK_AWDL_PEER_PROJECTION_H
#define HK_AWDL_PEER_PROJECTION_H
#include "peer_table.h"
/* Extrapolate a received periodic sequence only while that observed link
 * identity is live. This is a peer timing estimate, not a local RF grant;
 * the caller still requires the real firmware ROC or actual home basis. */
static inline bool awdl_peer_project(const struct lx_awdl_peer *p,uint64_t now,unsigned guard,struct awdl_window *out)
{
    if(!p || !p->serial || !p->clock.from_peer || now<p->last_rx_us || now-p->last_rx_us>=LX_AWDL_PEER_TTL_US)
        return false;
    struct awdl_window_clock clock=p->clock;
    uint64_t limit=p->last_rx_us+LX_AWDL_PEER_TTL_US;
    if(clock.valid_until_us<limit)clock.valid_until_us=limit;
    return awdl_window_at(&clock,now,guard,out);
}

/* OWL schedule.c awdl_is_multicast_eaw uses the synchronized master EAW,
 * not an interval starting when this local radio owner was constructed.
 * A received counter's nonzero base matters even when the phase matches.
 * This timing estimate never substitutes for the caller's actual RF grant. */
static inline bool awdl_peer_multicast_due(const struct lx_awdl_peer *p,uint64_t now)
{
    struct awdl_window w;
    if(!p || p->clock.period_tu!=16 || p->clock.presence!=4 ||
       !awdl_peer_project(p,now,16,&w) || !w.in_guarded_interval)return false;
    unsigned slot=(w.aw/4u)&15u;
    return slot==0 || slot==10;
}
#endif
