#include "lx_awdl_transport.h"
#define LIVE_DEPTH 8u
#define LIVE_MTU 1500u
struct live_frame { uint64_t lease; size_t n; uint8_t bytes[14+LIVE_MTU]; };
struct live_fifo { unsigned first,count; struct live_frame slot[LIVE_DEPTH]; };
static struct {
	struct lx_radio *radio;
	struct ieee80211_vif *vif;
	struct ieee80211_sta *peer;
	uint64_t generation,lease,flight_lease;
	struct live_fifo tx,rx;
	struct live_frame scratch;
	unsigned running,ended,stop,armed,accept,calls,registered,ready,closing;
	unsigned posted,tqm_ack,fw_release,failed_input,delivered,dropped,rx_observed;
	unsigned group_posted,group_retired,configured_notice,queue_unavailable;
	unsigned seen_psf,seen_mif;
	unsigned long long anchor_us,last_action_us;
	unsigned seq,mgmt_pending,mgmt_count,mgmt_success,next_mif,mgmt_fw,mgmt_status;
	struct sk_buff *mgmt_frame;
	unsigned mgmt_desc,mgmt_bound,mgmt_submitted,mgmt_phy;
	int mgmt_rc;
} live;
static bool live_mac_local(const uint8_t *a) { return !memcmp(a,echo.mac,6); }
static bool live_mac_peer(const uint8_t *a) { return !memcmp(a,awdl_target_mac,6); }
static bool live_group(const uint8_t *da,const uint8_t *ip)
{
	return da[0]==0x33 && da[1]==0x33 && ip[24]==0xff && (ip[25]&15)==2 && !memcmp(da+2,ip+36,4);
}
static bool live_current(uint64_t gen,uint64_t lease)
{ return live.registered && live.generation==gen && live.lease==lease && lease; }
static bool live_put(struct live_fifo *queue,uint64_t lease,const uint8_t *frame,size_t n)
{
	if (queue->count==LIVE_DEPTH || n>sizeof(queue->slot[0].bytes)) return false;
	struct live_frame *p=&queue->slot[(queue->first+queue->count)%LIVE_DEPTH];
	p->lease=lease;p->n=n;memcpy(p->bytes,frame,n);queue->count++;
	return true;
}
static void live_pop(struct live_fifo *queue)
{ queue->first=(queue->first+1)%LIVE_DEPTH;queue->count--; }
static int live_open(void *ctx,uint64_t gen,uint64_t lease,struct hk_awdl_link_config *cfg)
{
	if (ctx!=&live || !cfg || !lease) return EINVAL;
	lx_os_lock();
	int e=0;
	if (live.generation!=gen || !live.ready || !live.armed || live.stop) e=ENETDOWN;
	else if (live.lease && live.lease!=lease) e=EBUSY;
	else {
		live.lease=lease;live.accept=1;live.closing=0;live.configured_notice=0;
		memcpy(cfg->address,echo.mac,6);cfg->mtu=LIVE_MTU;
	}
	lx_os_unlock();
	lx_tell("awdl-live: open rc=%d mtu=%u\n",e,e?0:LIVE_MTU);
	return e;
}
static int live_output(void *ctx,uint64_t gen,uint64_t lease,const uint8_t *frame,size_t n)
{
	if (ctx!=&live || !frame || n<54 || n>14+LIVE_MTU || frame[12]!=0x86 || frame[13]!=0xdd ||
	    !lx_awdl_valid_ipv6(frame+14,n-14) || !live_mac_local(frame+6)) return EINVAL;
	if (!live_group(frame,frame+14) && (!live_mac_peer(frame) || memcmp(frame+38,awdl_target_ip,16))) return EHOSTUNREACH;
	lx_os_lock();
	int e=0;
	if (!live_current(gen,lease) || !live.accept || live.stop) e=ENETDOWN;
	else if (!live_put(&live.tx,lease,frame,n)) { live.queue_unavailable++;e=ENOBUFS; }
	lx_os_unlock();
	return e;
}
static int live_channels(void *ctx,uint64_t gen,uint64_t lease,struct hk_awdl_channels *out)
{
	if (ctx!=&live || !out) return EINVAL;
	lx_os_lock();
	int e=(!live_current(gen,lease)||!live.armed)?ENETDOWN:0;
	if (!e) { memset(out,0,sizeof(*out));out->preferred=6; }
	lx_os_unlock();
	return e;
}
static int live_quiesce(void *ctx,uint64_t gen,uint64_t lease)
{
	if (ctx!=&live) return EINVAL;
	lx_os_lock();
	if (!live.lease) { lx_os_unlock();return 0; }
	if (!live_current(gen,lease)) { lx_os_unlock();return ENOENT; }
	live.accept=0;live.closing=1;
	live.dropped+=live.tx.count+live.rx.count;
	live.tx.count=live.rx.count=0;
	int e=(live.calls || live.flight_lease==lease)?EBUSY:0;
	lx_os_unlock();
	return e;
}
static int live_close(void *ctx,uint64_t gen,uint64_t lease)
{
	int e=live_quiesce(ctx,gen,lease);
	if (e) return e;
	lx_os_lock();if (live.lease==lease) {live.lease=0;live.configured_notice=0;}lx_os_unlock();
	return 0;
}
static const struct hk_awdl_transport_ops live_ops={
	.open=live_open,.transmit=live_output,.channels=live_channels,.quiesce=live_quiesce,.close=live_close
};
static unsigned live_once_rx_ip;
static bool live_rx_ipv6(const uint8_t *da,const uint8_t *ip,size_t n)
{
	if (!EG(live.running) || !EG(live.armed) || !lx_awdl_valid_ipv6(ip,n) || n>LIVE_MTU ||
	    (!live_mac_local(da) && !live_group(da,ip))) return false;
	/* Caller has already validated the provisioned radio peer and BSSID. */
	lx_os_lock();
	if (!memcmp(ip+8,awdl_target_ip,16)) ES(live_once_rx_ip,1);
	if (!live.accept || !live.lease) { lx_os_unlock();return true; }
	if (live.rx.count==LIVE_DEPTH) {live.dropped++;lx_os_unlock();return true;}
	struct live_frame *pkt=&live.rx.slot[(live.rx.first+live.rx.count)%LIVE_DEPTH];
	pkt->lease=live.lease;pkt->n=14+n;
	memcpy(pkt->bytes,da,6);memcpy(pkt->bytes+6,awdl_target_mac,6);
	pkt->bytes[12]=0x86;pkt->bytes[13]=0xdd;memcpy(pkt->bytes+14,ip,n);
	live.rx.count++;live.rx_observed++;lx_os_unlock();return true;
}
static void live_deliver(void)
{
	for (unsigned budget=0;budget<LIVE_DEPTH;budget++) {
		lx_os_lock();
		if (!live.rx.count || !live.accept) {lx_os_unlock();return;}
		struct live_frame *f=&live.rx.slot[live.rx.first];
		live.scratch=*f;live_pop(&live.rx);live.calls++;
		uint64_t g=live.generation;
		lx_os_unlock();
		int e=lx_os_awdl_input(g,live.scratch.lease,live.scratch.bytes,live.scratch.n);
		lx_os_lock();live.calls--;if (e) live.failed_input++;else live.delivered++;lx_os_unlock();
	}
}
static int live_role(struct lx_radio *r) { return r->persistent_awdl; }
static unsigned live_flight_group;
static unsigned live_wmi_psf,live_wmi_mif,live_wmi_drop,live_wmi_errors;
static unsigned live_verified_notice;
static unsigned long long live_first_us,live_last_notice;
static unsigned live_infra_window;
static struct ieee80211_vif *live_window_vif(struct lx_radio *r,struct ieee80211_vif *v) { return r->persistent_awdl && live_infra_window ? r->vif : v; }
/* Called only by the long-lived radio owner, never by the VDEV's IRQ. */
static void live_retire(void)
{
	if (!EG(live.flight_lease) || !EG(echo.released)) return;
	lx_os_lock();
	if (live_flight_group) live.group_retired+=!!EG(echo.wbm);
	else live.tqm_ack+=!!EG(echo.acked);
	live.fw_release+=!!EG(echo.wbm);
	if (!EG(echo.wbm) || (!live_flight_group && !EG(echo.acked))) live.dropped++;
	ES(live.flight_lease,0);lx_os_unlock();
}
static void live_data(struct lx_radio *r,struct ieee80211_vif *v,
    struct linuxkpi_ieee80211_channel *ch,unsigned basis)
{
	live_retire();
	if (!basis || EG(live.flight_lease) || EG(echo.msdu) || !EG(live.accept)) return;
	lx_os_lock();
	if (!live.tx.count || !live.accept || live.stop) {lx_os_unlock();return;}
	struct live_frame *f=&live.tx.slot[live.tx.first];
	live.scratch=*f;live_pop(&live.tx);
	ES(live.flight_lease,live.scratch.lease);
	lx_os_unlock();
	uint8_t *frame;
	size_t payload=live.scratch.n-14;
	struct sk_buff *skb=frame_new(r,(unsigned)(40+payload),&frame);
	if (!skb) { ES(live.flight_lease,0);EA(live.dropped);return; }
	memset(frame,0,24);frame[0]=0x08;
	memcpy(frame+4,live.scratch.bytes,6);memcpy(frame+10,v->addr,6);memcpy(frame+16,awdl_network,6);
	unsigned seq=++live.seq;frame[22]=(uint8_t)(seq<<4);frame[23]=(uint8_t)(seq>>4);
	memcpy(frame+40,live.scratch.bytes+14,payload);
	if (!lx_awdl_ipv6_encap(frame+24,16,(uint16_t)seq,frame+40,payload)) {
		dev_kfree_skb_any(skb);ES(live.flight_lease,0);EA(live.dropped);return;
	}
	struct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);ti->control.vif=v;ti->band=ch->band;
	live_flight_group=!!(frame[4]&1);
	ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|(live_flight_group?IEEE80211_TX_CTL_NO_ACK:0);
	struct ieee80211_tx_control control={.sta=live.peer};
	ES(echo.bound,0);ES(echo.wbm,0);ES(echo.acked,0);ES(echo.released,0);ES(echo.eaten,0);
	ES(echo.slots,0);ES(echo.timedout,0);ES(echo.submitted,1); ES(echo.msdu,skb);
	ES(echo.submit_us,lx_os_nanoseconds()/1000ull);
	EA(live.posted);if (live_flight_group) EA(live.group_posted);
	r->ops->tx(&r->hw,&control,skb);
}
static int live_mgmt_prepare(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned id,unsigned desc,unsigned freq)
{
	if (EG(live.mgmt_frame)!=skb) return 0;
	if (!EG(live.armed) || live.radio!=radio_of(hw) || id!=echo.vdev || freq!=2437 ||
	    !lx_awdl_data_foreign(hw) || EG(live.stop)) return -ECANCELED;
	ES(live.mgmt_desc,desc);ES(live.mgmt_bound,1);return 1;
}
static void live_discovery(struct lx_radio *r,struct ieee80211_vif *v,struct linuxkpi_ieee80211_channel *ch)
{
	uint64_t now=lx_os_nanoseconds()/1000ull, began=EG(r->roc.foreign_us);
	if (EG(live.mgmt_pending) || EG(live.mgmt_frame) || !lx_awdl_data_foreign(&r->hw) ||
	    !began || now<began || now-began>420000 || (live.last_action_us && now-live.last_action_us<110*1024)) return;
	uint8_t data[LX_AWDL_DISCOVERY_MAX];
	struct lx_awdl_discovery_window w={.now_us=now,.anchor_us=live.anchor_us,.grant_end_us=began+475000,
	    .ieee_sequence=(uint16_t)((live.seq+1)&0xfff),.af_period_tu=110};
	const struct ieee80211_sta_ht_cap *ht=&r->wiphy->bands[ch->band]->ht_cap;
	struct lx_awdl_discovery_ht cap={.capabilities=ht->cap,.ampdu_parameters=(uint8_t)((ht->ampdu_density<<2)|ht->ampdu_factor),
	    .rx_mcs0=ht->mcs.rx_mask[0]};
	unsigned subtype=live.next_mif?3:0;
	unsigned n=(unsigned)lx_awdl_discovery_action(data,sizeof(data),v->addr,(uint8_t)subtype,&w,ht->ht_supported?&cap:NULL);
	if (!n) return;
	uint8_t *p;struct sk_buff *skb=frame_new(r,n,&p);if (!skb) return;
	memcpy(p,data,n);live.seq++;
	struct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);ti->control.vif=v;ti->band=ch->band;
	ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|IEEE80211_TX_CTL_TX_OFFCHAN|IEEE80211_TX_CTL_NO_ACK;
	struct ieee80211_tx_control ctrl={.sta=NULL};
	live.last_action_us=now;ES(live.mgmt_phy,subtype);ES(live.mgmt_fw,0);ES(live.mgmt_bound,0);
	ES(live.mgmt_pending,1);ES(live.mgmt_frame,skb);r->ops->tx(&r->hw,&ctrl,skb);
}
static void live_presence(void)
{
	uint64_t now=lx_os_nanoseconds()/1000ull,g,l;
	if (now-live_last_notice<250000 || !EG(live.accept)) return;
	live_last_notice=now;
	lx_os_lock();g=live.generation;l=live.lease;lx_os_unlock();
	if (!l) return;
	if (!EG(live.configured_notice)) {
		int e=lx_os_awdl_peer_present(g,l,HK_AWDL_PEER_PROVISIONED,awdl_target_mac,awdl_target_ip);
		if (!e) { ES(live.configured_notice,1);lx_tell("awdl-live: scoped_peer_cache_provisioned=1\n"); }
	}
	if (EG(live_once_rx_ip) && !EG(live_verified_notice)) {
		int e=lx_os_awdl_peer_present(g,l,HK_AWDL_PEER_OVER_AIR_VERIFIED,awdl_target_mac,awdl_target_ip);
		if (!e) ES(live_verified_notice,1);
	}
}
static void live_tick(struct lx_radio *r,struct ieee80211_vif *v,struct linuxkpi_ieee80211_channel *ch,unsigned basis)
{
	if (!EG(live.armed) || EG(live.stop)) return;
	live_retire();live_deliver();live_presence();
	if (basis==1 && live_infra_window) live_discovery(r,v,ch);
	if (basis && EG(live.accept)) live_data(r,v,ch,basis);
}
static bool live_service(struct lx_radio *r,struct ieee80211_vif *v,struct ieee80211_sta *peer,
    struct linuxkpi_ieee80211_channel *ch)
{
	live.radio=r;live.vif=v;live.peer=peer;live.anchor_us=lx_os_nanoseconds()/1000ull;
	live_first_us=live.anchor_us;ES(live.armed,1);ES(echo.receive,1);
	int e=lx_os_awdl_register(&live_ops,&live,&live.generation);
	if (e) {ES(live.armed,0);ES(echo.receive,0);lx_tell("awdl-live: register rc=%d\n",e);return true;}
	ES(live.registered,1);
	lx_tell("awdl-live: owner running static_peer=1 carrier_active=1 mtu=%u\n",LIVE_MTU);
	unsigned cycles=0;
	while (!EG(live.stop)) {
		/* Discovery requests ch6 from the real infra owner on its other
		 * channel; DATA still uses the independently mapped direct VDEV. */
		live_infra_window=(++cycles%4)!=0;
		memset(&bw,0,sizeof(bw));
		int outcome=lx_awdl_station_window(r,v,ch);
		if (outcome<0 || !lx_awdl_roc_driver_idle(&r->hw)) {ES(live.stop,1);break;}
		live_retire();live_deliver();live_presence();
		if (!EG(live.ready) && EG(live_wmi_psf) && EG(live_wmi_mif) && !EG(live.stop)) {
			ES(live.ready,1);e=lx_os_awdl_set_ready(live.generation,true);
			if (e) ES(live.ready,0);
			lx_tell("awdl-live: ready rc=%d psf_fw=%u mif_fw=%u static_peer=1\n",e,EG(live_wmi_psf),EG(live_wmi_mif));
		}
		lx_tell("awdl-live: cycle=%u infra_roc=%u lease_open=%u psf_fw=%u mif_fw=%u mgmt_drop=%u mgmt_error=%u tx_posted=%u tx_wbm=%u unicast_ack=%u group_posted=%u group_wbm=%u rx_queued=%u native_rx=%u native_input_err=%u discarded=%u\n",
		    cycles,live_infra_window,!!EG(live.lease),EG(live_wmi_psf),EG(live_wmi_mif),EG(live_wmi_drop),EG(live_wmi_errors),
		    EG(live.posted),EG(live.fw_release),EG(live.tqm_ack),EG(live.group_posted),EG(live.group_retired),
		    EG(live.rx_observed),EG(live.delivered),EG(live.failed_input),EG(live.dropped));
		/* Returning to infra remains part of each radio transaction. */
		for (unsigned i=0;i<12 && !EG(live.stop);i++) { live_retire();live_deliver();lx_pause_ms(10); }
	}
	ES(live.ready,0);(void)lx_os_awdl_set_ready(live.generation,false);
	lx_os_lock();live.accept=0;lx_os_unlock();
	unsigned long long limit=lx_os_nanoseconds()+1500000000ull;
	while ((EG(live.flight_lease)||EG(live.mgmt_pending)) && lx_os_nanoseconds()<limit) {live_retire();lx_pause_ms(10);}
	ES(live.armed,0);ES(echo.receive,0);
	/* The enclosing owner performs the targeted DOWN if a DATA WBM is
	 * outstanding, then retries unregister once its DMA reference retires. */
	return !EG(live.mgmt_pending);
}
static void live_withdraw(void)
{
	live_retire();
	if (!EG(live.registered)) return;
	unsigned long long limit=lx_os_nanoseconds()+1800000000ull;
	int e;
	do {e=lx_os_awdl_unregister(live.generation);if(!e) break;lx_pause_ms(10);} while(lx_os_nanoseconds()<limit);
	if(!e) ES(live.registered,0);
	lx_tell("awdl-live: unregister rc=%d tx_flight=%u\n",e,!!EG(live.flight_lease));
}
