/* Advertise to fresh observed peers even before they have generated TCP or
 * an outbound queue entry. The data carrier and the election owner stay live. */
static struct {uint64_t serial,visited;} discovery_visits[LX_AWDL_PEER_SLOTS];
static struct slot_target discovery_target;
static unsigned discovery_channel;

static unsigned discovery_visit(struct lx_radio *radio,unsigned carrier,uint64_t now)
{
    uint64_t oldest=UINT64_MAX;
    unsigned winner=0;
    struct slot_target best={0};
    lx_os_lock();
    for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++){
        const struct lx_awdl_peer *p=&air.table.peers[i];
        if(!p->serial || !p->seen_mif || !p->clock.from_peer || p->serial==air.active_serial ||
           now>=p->clock.valid_until_us)continue;
        uint64_t last=discovery_visits[i].serial==p->serial?discovery_visits[i].visited:0;
        if(last>oldest)continue;
        struct awdl_window window;
        uint64_t when=now;
        uint64_t score=0;
        struct slot_target plan={0};
        unsigned channel=0;
        for(unsigned k=0;k<16 && awdl_window_at(&p->clock,when,8,&window);k++,when=window.end_us){
            unsigned ch=window.channel;
            if(!ch || ch>=178 || ch==carrier || !auto_channel(radio,ch,true) ||
               roc_recently_unavailable(ch,now))continue;
            uint64_t start=window.start_us+8192,end=window.end_us-8192;
            unsigned lead=slot_lead(ch);
            uint64_t issue=start>now+lead?start-lead:now;
            if(issue-now>250000)continue;
            unsigned n=__atomic_load_n(&latencies[ch].count,__ATOMIC_ACQUIRE);
            uint64_t total=0;
            for(unsigned j=0;j<(n?n:1);j++){
                unsigned lag=n?__atomic_load_n(&latencies[ch].samples[j],__ATOMIC_RELAXED):lead;
                uint64_t arrival=issue+lag,lo=arrival>start?arrival:start;
                if(end>lo)total+=end-lo;
            }
            uint64_t value=(total/(n?n:1))*65536/(65536+issue-now);
            if(value<=score)continue;
            score=value;channel=ch;
            plan=(struct slot_target){start,end,issue,p->serial,lead};
        }
        if(!channel || (last==oldest && best.serial && best.issue<=plan.issue))continue;
        oldest=last;best=plan;winner=channel;
    }
    lx_os_unlock();
    discovery_target=best;discovery_channel=winner;
    return winner;
}
