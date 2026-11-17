#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define LIVE_DEPTH 8u
struct live_frame { uint8_t link_target[6],bytes[16]; };
struct live_fifo { unsigned first,count;struct live_frame slot[LIVE_DEPTH]; };
static uint8_t active_peer_mac[6]={2};
static struct { bool active;uint64_t active_serial; } air={true,17};
static bool auto_lane_target(const uint8_t *p) { return p[0]==active_peer_mac[0]; }
#include "peer_queue.inc"
static void put(struct live_fifo *q,unsigned offset,uint8_t target) {
    struct live_frame *f=&q->slot[(q->first+offset)%LIVE_DEPTH];
    f->link_target[0]=f->bytes[0]=target;
}
int main(void) {
    /* A wrapped queue with another recipient at its head must still send
     * the current peer's ACK, preserving that peer's packet order. */
    struct live_fifo q={.first=7,.count=4};
    put(&q,0,4);put(&q,1,2);put(&q,2,4);put(&q,3,2);
    assert(live_peer_offset(&q,false)==1);
    assert(live_peer_choice(&q,100)==1);
    assert(live_peer_choice(&q,1048675)==1);
    assert(live_peer_choice(&q,1048676)==0);
    /* A different queued peer cannot starve behind a busy active head. */
    put(&q,0,2);put(&q,1,2);
    assert(live_peer_choice(&q,1048676)==2);
    /* Rotation resets the quantum by canonical serial, not a timer tick. */
    air.active_serial=18;
    assert(live_peer_choice(&q,2000000)==0);
    assert(live_peer_choice(&q,3000000)==0);
    assert(live_peer_choice(&q,3048576)==2);
    put(&q,2,2);assert(live_peer_choice(&q,9999999)==0);
    /* Preserve destination validation, even if an entry's target is old. */
    q.slot[7].bytes[0]=4;assert(live_peer_offset(&q,false)==1);
    assert(live_peer_offset(&q,true)==0);
    for(unsigned i=0;i<q.count;i++)put(&q,i,4);
    assert(live_peer_offset(&q,false)==q.count);
    assert(live_peer_choice(&q,9999999)==0);
    air.active=false;assert(live_peer_choice(&q,9999999)==0);
    q.count=0;assert(live_peer_offset(&q,false)==0);
    return 0;
}
