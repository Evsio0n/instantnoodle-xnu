/* Preserve a real short-lived DNS lease while the original native client
 * is connecting. Retransmit the original query, never extend answer TTLs
 * or manufacture records. Only the currently active, fresh peer with real
 * TCP8770 traffic is eligible; native-query/transport leases bound retries. */
static void mdns_refresh_tick(void)
{
    if(!air.active || air.prepeer || !auto_active_fresh())return;
    uint64_t now=lx_os_nanoseconds()/1000ull;
    lx_os_lock();
    unsigned slot=air.active_slot;struct mdns_refresh_cache *cache=&mdns_refresh[slot];
    struct lx_awdl_peer *peer=&air.table.peers[slot];
    if(!live.accept || live.stop || !cache->n || cache->serial!=air.active_serial ||
       cache->lease!=live.lease || now<cache->wire_us || now-cache->wire_us>=30000000ull ||
       mdns_transfer_serial[slot]!=peer->serial || now>=mdns_transfer_until[slot] ||
       now<cache->next_us || live.tx.count) {lx_os_unlock();return;}
    if(live_put(&live.tx,live.lease,cache->bytes,cache->n)) {
        struct live_frame *frame=&live.tx.slot[(live.tx.first+live.tx.count-1)%LIVE_DEPTH];
        const uint8_t *ip=peer->has_observed_ipv6?peer->observed_ipv6:peer->eui64;
        if(!hk_mdns_query_unicast(frame->bytes,frame->n,peer->data_mac,ip)) {
            live.tx.count--;cache->n=0;lx_os_unlock();return;
        }
        frame->tag=++live.next_tag;frame->proto=LP_MDNS;frame->queued_us=now;
        memcpy(frame->link_target,peer->data_mac,6);cache->next_us=now+3000000ull;
        mdns_query_serial[slot]=peer->serial;mdns_query_until[slot]=now+15000000ull;
        if(++mdns_refresh_sent<=32)lx_tell("awdl-mdns: original_query_retry ticket=%u peer=%llu\n",frame->tag,peer->serial);
    }
    lx_os_unlock();
}
