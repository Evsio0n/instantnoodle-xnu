/* A receiving phone also needs receive windows when its TX FIFO is empty.
 * Track forward progress per observed peer and TCP source port. Repeated
 * packets cannot indefinitely extend the visit. Called under lx_os_lock. */
struct receive_progress {
    uint64_t serial,lease,until;
    unsigned port,end,bytes;
};
static struct receive_progress receive_peer[LX_AWDL_PEER_SLOTS];
static void receive_note(unsigned slot,const uint8_t *ip,size_t n,uint64_t now)
{
    if(n<60 || ip[6]!=6 || ip[42]!=0x22 || ip[43]!=0x42)return;
    unsigned header=(ip[52]>>4)*4;
    unsigned length=((unsigned)ip[4]<<8)|ip[5];
    if(header<20 || length<header || 40u+length>n)return;
    unsigned payload=length-header;

    unsigned port=((unsigned)ip[40]<<8)|ip[41];
    unsigned end=live_wire_u32(ip+44)+payload+!!(ip[53]&2);
    struct receive_progress *p=&receive_peer[slot];
    bool same=p->lease==live.lease && p->serial==air.table.peers[slot].serial && p->port==port;
    if(ip[53]&5){
        if(same){p->until=0;p->bytes=0;}
        return;
    }
    if(!payload && !(ip[53]&2))return;
    if(!same && p->lease==live.lease && p->serial==air.table.peers[slot].serial &&
       now<p->until && p->bytes>=65536)return;
    int32_t advance=(int32_t)(end-p->end);
    if(same && advance<=0)return;
    unsigned fresh=same && (unsigned)advance<payload?(unsigned)advance:payload;
    if(!same)p->bytes=0;
    if(p->bytes<65536)p->bytes+=fresh;
    p->serial=air.table.peers[slot].serial;p->lease=live.lease;
    p->port=port;p->end=end;
    /* A native upload can pause after its body while the HTTP transaction
     * finishes. Short Discover requests must not steal that carrier. */
    p->until=now+(p->bytes>=65536?30000000ull:2000000ull);
}

static int receive_choice(uint64_t now)
{
    int best=-1;
    for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++){
        const struct receive_progress *p=&receive_peer[i];
        if(p->until<=now || p->lease!=live.lease || !p->serial ||
           p->serial!=air.table.peers[i].serial)continue;
        bool bulk=p->bytes>=65536;
        if(best<0 || (bulk && receive_peer[best].bytes<65536) ||
           (bulk==(receive_peer[best].bytes>=65536) && p->until>receive_peer[best].until))best=(int)i;
    }
    return best;
}
