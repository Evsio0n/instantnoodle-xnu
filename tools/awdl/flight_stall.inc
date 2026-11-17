/* A firmware-owned raw-data command may never complete until VDEV down.
 * Do not let that command disable peer fairness forever. Request ordinary
 * carrier retirement; the existing vdev-down/drain path remains the sole
 * owner of DMA and skb reclamation. No descriptor is freed on this timer. */
static bool live_carrier_stalled(uint64_t now)
{
    bool stalled=false;unsigned ticket=0;uint64_t age=0;
    lx_os_lock();
    for(unsigned i=0;i<LIVE_FLIGHTS;i++) {
        const struct live_flight *f=&native_flights[i];
        if(f->lease && !EG(f->freed) && now>=f->send_us && now-f->send_us>=2000000ull) {
            stalled=true;ticket=f->tag;age=now-f->send_us;break;
        }
    }
    lx_os_unlock();
    if(stalled)lx_tell("awdl-flight-stall: ticket=%u age_us=%llu retire_carrier=1 free_descriptor=0\n",ticket,age);
    return stalled;
}
