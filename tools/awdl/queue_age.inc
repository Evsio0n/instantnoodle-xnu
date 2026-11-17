/* The OS retransmission clock continues while a radio peer is unavailable.
 * Never preserve its old ACK/SYN/data in a software FIFO for tens of seconds:
 * those stale copies can consume every future peer visit after the socket's
 * transaction has already timed out. Expire only unsent FIFO entries; the
 * firmware flights and their DMA owners are completely separate. */
static unsigned live_queue_expire(struct live_fifo *q,uint64_t now)
{
    unsigned kept=0,dropped=0;
    for(unsigned i=0;i<q->count;i++) {
        struct live_frame *f=&q->slot[(q->first+i)%LIVE_DEPTH];
        if(now>=f->queued_us && now-f->queued_us>=2000000ull) {dropped++;continue;}
        if(kept!=i)q->slot[(q->first+kept)%LIVE_DEPTH]=*f;
        kept++;
    }
    q->count=kept;
    return dropped;
}
static void live_queue_age_tick(uint64_t now)
{
    lx_os_lock();
    unsigned unicast=live_queue_expire(&live.tx,now),group=live_queue_expire(&live.groups,now);
    live.dropped+=unicast+group;
    lx_os_unlock();
    if(unicast || group)lx_tell("awdl-queue-age: unicast=%u group=%u limit_us=2000000 firmware_untouched=1\n",unicast,group);
}
