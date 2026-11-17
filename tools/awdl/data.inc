/* One IPv6 Echo Request to the administratively provisioned AWDL endpoint.
 * All diagnostic fields are scalar receipts. No generic packet submission
 * entry point is exposed by this trial. */
#include "lx_awdl_data.h"
#include "echo6.h"
#include "echo_target_private.h"

#define EG(f) __atomic_load_n(&(f),__ATOMIC_ACQUIRE)
#define ES(f,v) __atomic_store_n(&(f),(v),__ATOMIC_RELEASE)
#define EA(f) __atomic_fetch_add(&(f),1,__ATOMIC_ACQ_REL)
static const uint8_t awdl_network[6]={0,0x25,0,0xff,0x94,0x73};
/* One radio and one run. Async hooks inspect this fixed-lifetime storage;
 * they never dereference the borrowed sta, vif, ctx or skb from this state. */
static struct {
	struct ieee80211_hw *hw;
	struct ieee80211_chanctx_conf *ctx;
	struct sk_buff *msdu;
	unsigned vdev, active, receive, mapped, unmapped, start, start_status, stop;
	unsigned submitted, bound, ring, desc, native, wbm, source, reason, attempts;
	unsigned acked, released, local_drop, matched, received, home_watch, beacon;
	unsigned char mac[6],ip[16],nonce[16];
} echo;
int lx_awdl_echo_ctx(struct ieee80211_hw *hw,struct ieee80211_chanctx_conf *ctx)
{
	return EG(echo.active) && echo.hw==hw && echo.ctx==ctx;
}
int lx_awdl_echo_private(void) { return EG(echo.active); }
void lx_awdl_echo_peer(unsigned id,const unsigned char *peer,int map)
{
	if (!EG(echo.active) || echo.vdev!=id || memcmp(peer,awdl_target_mac,6)) return;
	if (map) EA(echo.mapped); else EA(echo.unmapped);
}
void lx_awdl_echo_vdev(struct ieee80211_hw *hw,unsigned id,int start,unsigned status)
{
	if (!EG(echo.active) || echo.hw!=hw || echo.vdev!=id) return;
	if (start) { ES(echo.start_status,status); EA(echo.start); } else EA(echo.stop);
}
void lx_awdl_echo_desc(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned vdev,
    unsigned ring,unsigned desc,int native)
{
	if (!EG(echo.active) || echo.hw!=hw || echo.vdev!=vdev || EG(echo.msdu)!=skb) return;
	ES(echo.ring,ring); ES(echo.desc,desc); ES(echo.native,native); ES(echo.bound,1);
}
void lx_awdl_echo_wbm(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned ring,
    unsigned desc,unsigned src,unsigned reason,unsigned attempts,int acked)
{
	if (!EG(echo.active) || echo.hw!=hw || !skb || EG(echo.msdu)!=skb ||
	    !EG(echo.bound) || EG(echo.ring)!=ring || EG(echo.desc)!=desc) return;
	ES(echo.source,src); ES(echo.reason,reason); ES(echo.attempts,attempts);
	ES(echo.acked,acked); EA(echo.wbm);
}
void lx_awdl_echo_release(struct ieee80211_hw *hw,struct sk_buff *skb,int local_drop)
{
	if (!EG(echo.active) || echo.hw!=hw || !skb) return;
	struct sk_buff *expect=skb;
	if (!__atomic_compare_exchange_n(&echo.msdu,&expect,NULL,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
	ES(echo.local_drop,local_drop); ES(echo.released,1);
}
static bool lx_awdl_echo_receive(struct lx_radio *radio,struct sk_buff *skb,
    const struct ieee80211_rx_status *s)
{
	const uint8_t *p=skb->data;
	size_t n=skb_headlen(skb);
	if (EG(echo.home_watch) && !(s->flag&RX_FLAG_8023) && n>=36 && p[0]==0x80 &&
	    radio->channel && radio->channel->def.chan && s->freq==radio->channel->def.chan->center_freq &&
	    !memcmp(p+16,radio->target.address,6)) EA(echo.beacon);
	if (!EG(echo.receive) || echo.hw!=&radio->hw ||
	    (s->flag&(RX_FLAG_MMIC_ERROR|RX_FLAG_FAILED_FCS_CRC|RX_FLAG_FAILED_PLCP_CRC))) return false;
	const uint8_t *ip;
	size_t bytes;
	if (s->flag&RX_FLAG_8023) {
		if (n<14 || memcmp(p,echo.mac,6) || memcmp(p+6,awdl_target_mac,6) ||
		    p[12]!=0x86 || p[13]!=0xdd) return false;
		ip=p+14; bytes=n-14;
	} else {
		if (n<24 || (p[0]!=0x08 && p[0]!=0x88) || (p[1]&0x47u) || (p[22]&15) ||
		    memcmp(p+4,echo.mac,6) || memcmp(p+10,awdl_target_mac,6) ||
		    memcmp(p+16,awdl_network,6)) return false;
		size_t h=p[0]==0x88?26:24;
		if (n<h || (h==26 && (p[24]&0x80))) return false;
		if (h==26 && (p[1]&0x80)) h+=4;
		struct lx_awdl_ipv6 ipv6;
		if (n<h || !lx_awdl_ipv6_decap(p+h,n-h,&ipv6)) return false;
		ip=ipv6.packet; bytes=ipv6.length;
	}
	EA(echo.received);
	if (hk_awdl_echo6_reply(ip,bytes,echo.ip,awdl_target_ip,echo.nonce)) ES(echo.matched,1);
	return true;
}
static bool lx_awdl_echo_trial(struct lx_radio *radio,struct ieee80211_vif *vif)
{
	struct ieee80211_hw *hw=&radio->hw;
	struct wiphy *w=hw->wiphy;
	const struct ieee80211_ops *ops=radio->ops;
	struct linuxkpi_ieee80211_channel *ch=linuxkpi_ieee80211_get_channel(w,2437);
	if (!ch || (ch->flags&(IEEE80211_CHAN_RADAR|IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_NO_IR)) ||
	    !ops->sta_state || !ops->tx || !ops->add_chanctx || !ops->assign_vif_chanctx ||
	    !ops->remove_chanctx || !ops->unassign_vif_chanctx ||
	    !memcmp(vif->addr,awdl_target_mac,6) || vif==radio->vif) return true;
	struct ieee80211_chanctx_conf *ctx=kzalloc(sizeof(*ctx)+hw->chanctx_data_size+64,GFP_KERNEL);
	void *mem=kzalloc(sizeof(struct ieee80211_sta)+hw->sta_data_size+64,GFP_KERNEL);
	if (!ctx || !mem) { kfree(ctx); kfree(mem); return true; }
	struct ieee80211_sta *peer=(void *)roundup2((uintptr_t)mem,64);
	memcpy(peer->addr,awdl_target_mac,6); memcpy(peer->deflink.addr,awdl_target_mac,6);
	peer->deflink.sta=peer; peer->link[0]=&peer->deflink;
	ctx->def.chan=ch; ctx->def.center_freq1=ch->center_freq; ctx->def.width=NL80211_CHAN_WIDTH_20;
	ctx->min_def=ctx->def; ctx->rx_chains_static=ctx->rx_chains_dynamic=1;
	vif->bss_conf.chanreq.oper=ctx->def;
	vif->bss_conf.bssid=awdl_network;
	echo.hw=hw; echo.ctx=ctx; echo.vdev=radio->roc.p2p_id; memcpy(echo.mac,vif->addr,6);
	echo.ip[0]=0xfe; echo.ip[1]=0x80; memcpy(echo.ip+8,vif->addr,3);
	echo.ip[8]^=2; echo.ip[11]=0xff; echo.ip[12]=0xfe; memcpy(echo.ip+13,vif->addr+3,3);
	get_random_bytes(echo.nonce,sizeof(echo.nonce));
	ES(echo.active,1);
	lx_tell("awdl-echo: begin configured_peer=1 vdev=%u chan=6 clock=host_monotonic_us time=%llu\n",
	    echo.vdev,lx_os_nanoseconds()/1000ull);
	mutex_lock(&w->mtx);
	int a=ops->add_chanctx(hw,ctx), b=-ECANCELED, c=-ECANCELED, d=-ECANCELED;
	if (!a) {
		vif->bss_conf.chanctx_conf=ctx;
		b=ops->assign_vif_chanctx(hw,vif,&vif->bss_conf,ctx);
		if (!b) c=ops->sta_state(hw,vif,peer,IEEE80211_STA_NOTEXIST,IEEE80211_STA_NONE);
	}
	mutex_unlock(&w->mtx);
	lx_tell("awdl-echo: chan_add=%d assign=%d peer_create=%d htt_map=%u fw_start=%u start_status=%u\n",
	    a,b,c,EG(echo.mapped),EG(echo.start),EG(echo.start_status));
	if (!c && EG(echo.mapped)==1 && EG(echo.start)==1 && !EG(echo.start_status)) {
		uint8_t *packet;
		struct sk_buff *skb=frame_new(radio,24+16+64u,&packet);
		if (skb) {
			bzero(packet,24); packet[0]=0x08;
			memcpy(packet+4,awdl_target_mac,6); memcpy(packet+10,vif->addr,6);
			memcpy(packet+16,awdl_network,6); packet[22]=0x10;
			if (hk_awdl_echo6_request(packet+40,64u,echo.ip,awdl_target_ip,echo.nonce) &&
			    lx_awdl_ipv6_encap(packet+24,16,1,packet+40,64u)) {
				struct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);
				ti->control.vif=vif; ti->band=ch->band; ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS;
				struct ieee80211_tx_control control={.sta=peer};
				ES(echo.receive,1); ES(echo.msdu,skb); ES(echo.submitted,1);
				ops->tx(hw,&control,skb);
			} else dev_kfree_skb_any(skb);
		}
	}
	unsigned long long until=lx_os_nanoseconds()+1600000000ull;
	while (EG(echo.submitted) && (!EG(echo.released) || !EG(echo.matched)) &&
	    lx_os_nanoseconds()<until) lx_pause_ms(10);
	ES(echo.receive,0);
	lx_tell("awdl-echo: mac_submit=%u native=%u descriptor_bound=%u ring=%u desc=%u wbm=%u source=%u reason=%u tries=%u hw_ack=%u skb_release=%u local_free_path=%u peer_ipv6=%u verified_reply=%u\n",
	    EG(echo.submitted),EG(echo.native),EG(echo.bound),EG(echo.ring),EG(echo.desc),EG(echo.wbm),
	    EG(echo.source),EG(echo.reason),EG(echo.attempts),EG(echo.acked),EG(echo.released),
	    EG(echo.local_drop),EG(echo.received),EG(echo.matched));
	if (EG(echo.submitted) && !EG(echo.released)) {
		lx_tell("awdl-echo: TX ownership unresolved, retaining VDEV and peer\n"); return false;
	}
	mutex_lock(&w->mtx);
	if (!c) d=ops->sta_state(hw,vif,peer,IEEE80211_STA_NONE,IEEE80211_STA_NOTEXIST);
	bool cleaned=(c || !d) && EG(echo.mapped)==EG(echo.unmapped) &&
	    (!EG(echo.start) || EG(echo.start_status) || EG(echo.stop));
	if (cleaned) {
		if (!b) ops->unassign_vif_chanctx(hw,vif,&vif->bss_conf,ctx);
		vif->bss_conf.chanctx_conf=NULL;
		if (!a) ops->remove_chanctx(hw,ctx);
	}
	mutex_unlock(&w->mtx);
	lx_tell("awdl-echo: peer_delete=%d htt_unmap=%u fw_stop=%u chan_released=%u\n",
	    d,EG(echo.unmapped),EG(echo.stop),cleaned);
	if (!cleaned) return false;
	ES(echo.active,0); kfree(ctx); kfree(mem);
	ES(echo.home_watch,1); until=lx_os_nanoseconds()+1000000000ull;
	while (!EG(echo.beacon) && lx_os_nanoseconds()<until) lx_pause_ms(10);
	ES(echo.home_watch,0);
	lx_tell("awdl-echo: home_beacon=%u station_joined=%u\n",EG(echo.beacon),radio->joined);
	return true;
}
