#include "peer_projection.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct lx_awdl_peer p={.serial=1,.last_rx_us=100000};
    assert(awdl_clock_local(&p.clock,p.last_rx_us,6));p.clock.from_peer=true;
    p.clock.slots[0]=48;
    struct awdl_window w;
    uint64_t now=p.last_rx_us+16ull*65536+8192;
    assert(now>p.clock.valid_until_us);
    assert(!awdl_window_at(&p.clock,now,8,&w));
    assert(awdl_peer_project(&p,now,8,&w) && w.channel==48 && w.in_guarded_interval);
    assert(awdl_peer_project(&p,now-1,8,&w) && !w.in_guarded_interval);
    assert(!awdl_peer_project(&p,p.last_rx_us+LX_AWDL_PEER_TTL_US,8,&w));
    assert(!awdl_peer_project(&p,p.last_rx_us-1,8,&w));
    p.clock.from_peer=false;assert(!awdl_peer_project(&p,now,8,&w));
    p.clock.from_peer=true;
    /* Same local phase, different observed master counter: local boot-slot0
     * must not open a group window when the actual master is in slot1. */
    p.clock.base_aw=4;
    assert(!awdl_peer_multicast_due(&p,p.last_rx_us+16384));
    assert(awdl_peer_multicast_due(&p,p.last_rx_us+9ull*65536+16384));
    assert(awdl_peer_multicast_due(&p,p.last_rx_us+15ull*65536+16384));
    assert(!awdl_peer_multicast_due(&p,p.last_rx_us+9ull*65536+16383));
    assert(!awdl_peer_multicast_due(&p,p.last_rx_us+9ull*65536+49152));
    p.clock.base_aw=40;
    assert(awdl_peer_multicast_due(&p,p.last_rx_us+16384));
    p.clock.base_aw=65532;
    assert(awdl_peer_multicast_due(&p,p.last_rx_us+65536+16384));
    assert(!awdl_peer_multicast_due(&p,p.last_rx_us+LX_AWDL_PEER_TTL_US));
    puts("PASS received periodic window projection, guards, wrap and live-identity bound");
}
