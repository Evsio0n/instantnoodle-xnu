/* The canonical peer can change its availability sequence while its data
 * identity remains live. Request an ordinary owner/carrier replacement at
 * the completed-window boundary; never mutate the active channel or its
 * DMA context from an RX callback. The normal owner consumes wanted_serial.
 * A queued request for another peer keeps its existing priority. */
static bool auto_follow_channel(void)
{
    if (!air.active || air.prepeer) return false;
    uint64_t now=lx_os_nanoseconds()/1000ull;
    lx_os_lock();
    struct lx_awdl_peer peer=air.table.peers[air.active_slot];
    if (peer.serial!=air.active_serial || !peer.clock.from_peer ||
        now<peer.last_rx_us || now-peer.last_rx_us>=LX_AWDL_PEER_TTL_US ||
        now>=peer.clock.valid_until_us ||
        (air.wanted_serial && air.wanted_serial!=air.active_serial)) {
        lx_os_unlock();return false;
    }
    /* A change in advertised occupancy is not a carrier withdrawal. Both
     * sparse and dense real Mac sequences can retain the current channel.
     * Rebuilding on every predominant-count change tears down RX/peer state
     * even though guarded data windows on this carrier remain valid. */
    if(auto_channel(air.radio,air.channel,true))
        for(unsigned i=0;i<16;i++)if(peer.clock.slots[i]==air.channel) {
            lx_os_unlock();return false;
        }
    unsigned channel=auto_phy_chan(&peer,now),old=air.channel;
    if (!channel || channel==old) {lx_os_unlock();return false;}
    air.wanted=air.active_slot;air.wanted_serial=air.active_serial;
    uint64_t serial=air.active_serial;
    lx_os_unlock();
    lx_tell("awdl-auto: retune_requested serial=%llu old=%u new=%u\n",serial,old,channel);
    return true;
}
