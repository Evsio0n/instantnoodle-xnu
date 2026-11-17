/* Only the serialized ROC worker updates the channel latency history. */
static struct { unsigned samples[5],count,next; } latencies[178];
static unsigned slot_lead(unsigned ch)
{
    unsigned sorted[5],n=latencies[ch].count;
    if(!n)return ch==6?13000:ch==48?2200:32000;
    for(unsigned i=0;i<n;i++){
        unsigned j=i,value=latencies[ch].samples[i];
        while(j && sorted[j-1]>value){sorted[j]=sorted[j-1];--j;}
        sorted[j]=value;
    }
    return sorted[n/2];
}
static void slot_sample(unsigned ch,unsigned us)
{
    unsigned pos=latencies[ch].next;
    latencies[ch].samples[pos]=us;
    latencies[ch].next=(pos+1)%5;
    if(latencies[ch].count<5)latencies[ch].count++;
}
struct slot_target { uint64_t start,end,issue,serial; unsigned lead; };
static struct slot_target slot_target_for(unsigned ch,uint64_t now)
{
    struct slot_target target={0};
    lx_os_lock();
    struct lx_awdl_peer p=air.table.peers[air.active_slot];
    bool active=air.active && air.active_serial==p.serial;
    lx_os_unlock();
    if(!active || ch>=178 || !p.clock.from_peer)return target;
    unsigned lead=slot_lead(ch);
    uint64_t arrival=now+lead;
    for(unsigned i=0;i<16;i++){
        struct awdl_window w;
        if(!awdl_window_at(&p.clock,arrival,8,&w))break;
        uint64_t start=w.start_us+8192,end=w.end_us-8192;
        if(w.channel==ch && arrival+8192<end){
            target.start=start;target.end=end;target.serial=p.serial;target.lead=lead;
            target.issue=start>arrival?start-lead:now;
            return target;
        }
        arrival=w.end_us;
    }
    return target;
}
