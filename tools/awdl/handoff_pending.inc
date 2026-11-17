/* The active owner has already picked a recipient. Re-evaluating a group
 * FIFO after teardown loses the priority decision because air.active is
 * now false. Let auto_pick consume the decision after its own validation. */
static bool auto_pending_handoff(void)
{
    uint64_t now=lx_os_nanoseconds()/1000ull;
    lx_os_lock();
    unsigned slot=air.wanted;
    bool valid=air.wanted_serial && slot<LX_AWDL_PEER_SLOTS &&
        air.table.peers[slot].serial==air.wanted_serial &&
        !(lx_awdl_peer_expired(&air.table,now)&(1u<<slot)) &&
        auto_phy_chan(&air.table.peers[slot],now);
    if (!valid) air.wanted_serial=0;
    lx_os_unlock();
    return valid;
}
