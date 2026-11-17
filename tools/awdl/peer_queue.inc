/* The existing mutex protects the queue and canonical active peer. Preserve
 * FIFO within each peer while allowing its ACK/data behind another peer's
 * head entry to use the already configured carrier. No packet is rewritten. */
static unsigned live_peer_offset(const struct live_fifo *q, bool group)
{
    for (unsigned i=0;i<q->count;i++) {
        const struct live_frame *f=&q->slot[(q->first+i)%LIVE_DEPTH];
        if (!memcmp(f->link_target,active_peer_mac,6) &&
            (group || auto_lane_target(f->bytes))) return i;
    }
    return q->count;
}

/* A busy peer may batch for one schedule cycle, then the oldest different
 * peer receives the next carrier. Called only at completed ROC boundaries;
 * never relabel an interface, a DMA descriptor or an in-flight packet here. */
static uint64_t peer_batch_serial,peer_batch_until;
static unsigned live_peer_choice(const struct live_fifo *q,uint64_t now)
{
    if (!air.active || !air.active_serial) return 0;
    if (peer_batch_serial!=air.active_serial) {
        peer_batch_serial=air.active_serial;
        peer_batch_until=now+1048576ull;
    }
    unsigned active=live_peer_offset(q,false);
    if (active==q->count) return 0;
    if (now<peer_batch_until) return active;
    for (unsigned i=0;i<q->count;i++)
        if (memcmp(q->slot[(q->first+i)%LIVE_DEPTH].link_target,active_peer_mac,6)) return i;
    return active;
}
