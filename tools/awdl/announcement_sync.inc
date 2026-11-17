/* TX carrier follows pending traffic. Election is shared by all destinations.
 * Carry an observed clock only during its peer's bounded lifetime, and
 * reject a synchronization path that already leads through this station. */
static int announcement_sync_locked(uint64_t now)
{
    int best=-1;
    for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++){
        const struct lx_awdl_peer *p=&air.table.peers[i];
        if(!p->serial || !p->seen_mif || !p->version || !p->device_class ||
           !p->clock.from_peer || p->clock.period_tu!=16 || p->clock.presence!=4 ||
           p->distance>=9 || now<p->last_rx_us || now-p->last_rx_us>=LX_AWDL_PEER_TTL_US ||
           !memcmp(p->master,air.local,6) || !memcmp(p->sync_master,air.local,6) ||
           !memcmp(p->mac,air.local,6))continue;
        if(best<0 || lx_awdl_peer_rank(p,&air.table.peers[best])>0)best=(int)i;
    }
    return best;
}
