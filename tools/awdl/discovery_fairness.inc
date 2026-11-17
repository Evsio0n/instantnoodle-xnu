/* Called under the native FIFO mutex, at completed ROC boundaries only.
 * A new copy for the active peer must not pin older discovery copies for
 * other real peers indefinitely. Keep a minimum carrier slice and let any
 * in-flight firmware owner finish before choosing another recipient. */
static uint64_t discovery_slice_generation,discovery_slice_serial,discovery_slice_since;
static unsigned discovery_slice_channel;
static unsigned live_group_urgent_choice(const struct live_fifo *q,uint64_t now,bool in_flight)
{
    if (!air.active || !air.active_serial) return q->count;
    if (discovery_slice_generation!=live.generation ||
        discovery_slice_serial!=air.active_serial || discovery_slice_channel!=air.channel) {
        discovery_slice_generation=live.generation;
        discovery_slice_serial=air.active_serial;discovery_slice_channel=air.channel;
        discovery_slice_since=now;
    }
    if (in_flight || now<discovery_slice_since || now-discovery_slice_since<1048576ull)
        return q->count;
    for (unsigned i=0;i<q->count;i++) {
        const struct live_frame *f=&q->slot[(q->first+i)%LIVE_DEPTH];
        if (f->proto==LP_MDNS && now>=f->queued_us && now-f->queued_us>=2000000ull &&
            memcmp(f->link_target,active_peer_mac,6) && auto_find_locked(f->link_target,now)>=0)
            return i;
    }
    return q->count;
}
