#define LIVE_LANES 2u
#define LIVE_PER_LANE 2u
#define LIVE_FLIGHTS (LIVE_LANES*LIVE_PER_LANE)
struct live_flight {
	uint64_t lease,queued_us,send_us,bound_us,wbm_us,free_us;
	unsigned tag,proto,group,basis,bound,ring,desc,native,wbm,source,reason,tries,acked,free_path,freed;
	struct sk_buff *skb;
};
static struct live_flight native_flights[LIVE_FLIGHTS];
static unsigned live_next_lane,live_next_tag;
static unsigned live_count_flights_locked(uint64_t lease)
{
	unsigned count=0;
	for(unsigned i=0;i<LIVE_FLIGHTS;i++)
		if (native_flights[i].lease && (!lease || lease==native_flights[i].lease)) count++;
	return count;
}
static unsigned live_count_flights(uint64_t lease)
{
	lx_os_lock();unsigned count=live_count_flights_locked(lease);lx_os_unlock();return count;
}
static struct live_flight *live_find_skb(struct ieee80211_hw *hw,struct sk_buff *skb)
{
	if (!skb || !EG(live.running) || echo.hw!=hw) return NULL;
	for(unsigned i=0;i<LIVE_FLIGHTS;i++) if (EG(native_flights[i].skb)==skb) return &native_flights[i];
	return NULL;
}
static void live_bound(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned vdev,unsigned ring,unsigned desc,int native)
{
	struct live_flight *f=live_find_skb(hw,skb);
	if (!f || vdev!=echo.vdev) return;
	ES(f->ring,ring);ES(f->desc,desc);ES(f->native,!!native);ES(f->bound_us,lx_os_nanoseconds()/1000ull);ES(f->bound,1);
}
static void live_wbm(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned ring,unsigned desc,unsigned source,unsigned reason,unsigned tries,int ack)
{
	struct live_flight *f=live_find_skb(hw,skb);
	if (!f || !EG(f->bound) || ring!=EG(f->ring) || desc!=EG(f->desc)) return;
	ES(f->source,source);ES(f->reason,reason);ES(f->tries,tries);ES(f->acked,!!ack);
	ES(f->wbm_us,lx_os_nanoseconds()/1000ull);ES(f->wbm,1);
}
static void live_freed(struct ieee80211_hw *hw,struct sk_buff *skb,int path)
{
	struct live_flight *f=live_find_skb(hw,skb);
	if (!f) return;
	struct sk_buff *expected=skb;
	if (!__atomic_compare_exchange_n(&f->skb,&expected,NULL,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
	ES(f->free_path,path);ES(f->free_us,lx_os_nanoseconds()/1000ull);ES(f->freed,1);
}
static void live_retire(void)
{
	for(unsigned i=0;i<LIVE_FLIGHTS;i++) {
		struct live_flight snapshot;
		lx_os_lock();
		struct live_flight *f=&native_flights[i];
		if (!f->lease || !EG(f->freed)) {lx_os_unlock();continue;}
		snapshot=*f;
		if (f->group) live.group_retired+=!!f->wbm;
		else live.tqm_ack+=!!f->acked;
		live.fw_release+=!!f->wbm;
		if (!f->wbm || (!f->group && !f->acked)) live.dropped++;
		f->lease=0;lx_os_unlock();
		lx_tell("awdl-ntx: retire ticket=%u class=%s group=%u wbm=%u src=%u reason=%u tries=%u tqm_ack=%u free_path=%u bound=%u ring=%u desc=%u queue_us=%llu fw_us=%llu age_us=%llu\n",
		    snapshot.tag,live_proto_name(snapshot.proto),snapshot.group,snapshot.wbm,snapshot.source,snapshot.reason,snapshot.tries,
		    !!(snapshot.acked&&!snapshot.group),snapshot.free_path,snapshot.bound,snapshot.ring,snapshot.desc,snapshot.send_us-snapshot.queued_us,
		    snapshot.wbm?snapshot.wbm_us-snapshot.send_us:0,snapshot.free_us-snapshot.queued_us);
	}
}
static void live_data(struct lx_radio *r,struct ieee80211_vif *v,
    struct linuxkpi_ieee80211_channel *ch,unsigned basis)
{
	if (!basis || !EG(live.accept)) return;
	uint64_t now=lx_os_nanoseconds()/1000ull;
	struct live_flight *f=NULL;
	lx_os_lock();
	if (!live.accept || live.stop) {lx_os_unlock();return;}
	for(unsigned k=0;k<LIVE_LANES&&!f;k++) {
		unsigned lane=(live_next_lane+k)%LIVE_LANES;
		struct live_fifo *q=lane?&live.groups:&live.tx;
		if (!q->count || (lane&&!live_multicast_due(live.anchor_us,now))) continue;
		for(unsigned j=0;j<LIVE_PER_LANE;j++) {
			struct live_flight *next=&native_flights[lane*LIVE_PER_LANE+j];
			if (next->lease) continue;
			f=next;
			live.scratch=q->slot[q->first];live_pop(q);
			memset(f,0,sizeof(*f));f->lease=live.scratch.lease;f->tag=live.scratch.tag;
			f->proto=live.scratch.proto;f->group=lane;f->queued_us=live.scratch.queued_us;
			f->send_us=now;f->basis=basis;live_next_lane=1u-lane;
			break;
		}
	}
	lx_os_unlock();
	if (!f) return;
	size_t bytes=live.scratch.n-14;
	uint8_t *frame;struct sk_buff *skb=frame_new(r,(unsigned)(40+bytes),&frame);
	if (!skb) { ES(f->free_path,2); ES(f->free_us,now); ES(f->freed,1);return; }
	memset(frame,0,24);frame[0]=8;memcpy(frame+4,live.scratch.bytes,6);
	memcpy(frame+10,v->addr,6);memcpy(frame+16,awdl_network,6);
	unsigned seq=++live.seq;frame[22]=(uint8_t)(seq<<4);frame[23]=(uint8_t)(seq>>4);
	memcpy(frame+40,live.scratch.bytes+14,bytes);
	if (!lx_awdl_ipv6_encap(frame+24,16,(uint16_t)seq,frame+40,bytes)) {
		dev_kfree_skb_any(skb);ES(f->free_path,2);ES(f->free_us,now);ES(f->freed,1);return;
	}
	struct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);
	ti->control.vif=v;ti->band=ch->band;
	ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|(f->group?IEEE80211_TX_CTL_NO_ACK:0);
	struct ieee80211_tx_control control={.sta=live.peer};
	ES(f->skb,skb);EA(live.posted);if (f->group) EA(live.group_posted);
	r->ops->tx(&r->hw,&control,skb);
	lx_tell("awdl-ntx: mac_submit ticket=%u class=%s group=%u basis=%u bound=%u ring=%u desc=%u queue_us=%llu\n",
	    f->tag,live_proto_name(f->proto),f->group,basis,EG(f->bound),EG(f->ring),EG(f->desc),now-f->queued_us);
}
