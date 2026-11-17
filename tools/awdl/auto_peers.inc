/* Canonical radio observations only. IPv6 derived from a live L2 peer is
 * kept distinct from IPv6 actually carried by that peer's wireless frame. */
#include "peer_table.h"
static struct {
	struct lx_awdl_peer_table table;
	struct awdl_rx_action scratch;
	struct lx_radio *radio;
	struct ieee80211_vif *vif;
	unsigned running,decode_busy,psf,mif,ignored,bad,full,rv6,expired,switched;
	unsigned channel,mhz,active_slot,wanted,active,listen_mhz,prepeer;
	uint64_t active_serial,wanted_serial,lease_seen,last_maint_us,home_us,unicast_until_us;
	uint8_t local[6];
	struct {uint64_t serial;unsigned l2,ipv6;uint8_t mac[6],ip[16];} notice[8];
} air;
static uint8_t active_peer_mac[6],active_peer_ip[16];
static struct {
	struct ieee80211_hw *hw;
	struct ieee80211_vif *vif;
	struct ieee80211_sta *sta;
	void *storage;
	uint8_t mac[6];
	unsigned pending,configured,created,authorized;
} direct;
static void auto_direct_config_event(struct ieee80211_hw *hw,unsigned vdev,const uint8_t *mac)
{
	if(EG(direct.pending) && direct.hw==hw && vdev==echo.vdev && mac && !memcmp(direct.mac,mac,6)) EA(direct.configured);
}
int lx_awdl_direct_configured(struct ieee80211_hw *hw,unsigned vdev,const unsigned char *mac)
{
	return EG(direct.pending) && direct.hw==hw && vdev==echo.vdev && mac && !memcmp(direct.mac,mac,6) && EG(direct.configured)>0;
}
static unsigned auto_freq(unsigned channel)
{
	if (channel>=1 && channel<=13) return 2407+5*channel;
	if (channel==14) return 2484;
	if (channel>=32 && channel<=177) return 5000+5*channel;
	return 0;
}
static bool auto_channel(struct lx_radio *r,unsigned channel,bool tx)
{
	unsigned mhz=auto_freq(channel);
	if (!mhz) return false;
	struct linuxkpi_ieee80211_channel *ch=linuxkpi_ieee80211_get_channel(r->wiphy,mhz);
	if (!ch || ch->flags&IEEE80211_CHAN_DISABLED) return false;
	return !tx || !(ch->flags&(IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_RADAR));
}
static int auto_find_locked(const uint8_t mac[6],uint64_t now)
{
	uint8_t expired=lx_awdl_peer_expired(&air.table,now);
	for(unsigned i=0;i<8;i++) {
		const struct lx_awdl_peer *p=&air.table.peers[i];
		if (p->serial && !(expired&(1u<<i)) && !memcmp(p->data_mac,mac,6)) return (int)i;
	}
	return -1;
}
static bool auto_route(const uint8_t mac[6],const uint8_t *ip)
{
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();int i=auto_find_locked(mac,now);
	bool ok=i>=0 && (!ip || !memcmp(air.table.peers[i].has_observed_ipv6?
	    air.table.peers[i].observed_ipv6:air.table.peers[i].eui64,ip,16));
	lx_os_unlock();return ok;
}
static void auto_observe(struct ieee80211_hw *hw,unsigned mhz,const void *mpdu,size_t n)
{
	if (!EG(air.running) || !air.radio || hw!=&air.radio->hw || !mpdu || !mhz ||
	    n<40 || n>2304 || ((const uint8_t *)mpdu)[0]!=0xd0) return;
	const uint8_t *p=mpdu;size_t h=(p[1]&0x80)?28:24;
	if (n<h+16 || p[h]!=127 || p[h+1] || p[h+2]!=0x17 || p[h+3]!=0xf2 || p[h+4]!=8) return;
	unsigned expected=0;
	if(!__atomic_compare_exchange_n(&air.decode_busy,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();
	int i=lx_awdl_peer_observe(&air.table,p,n,air.local,mhz,now,&air.scratch);
	if(i>=0) {
		if (p[h+6]==3) {
			unsigned count=EA(air.mif)+1;
			if(count<5 || !(count%64)) {
				const struct lx_awdl_peer *peer=air.table.peers+i;
				lx_tell("awdl-auto: mif=%u serial=%llu class=%u data_address_present=%u data_equals_mgmt=%u heard_mhz=%u\n",
				    count,peer->serial,peer->device_class,peer->data_address_present,peer->data_address_matches,mhz);
			}
		}else EA(air.psf);
	}
	else if(i==-3) EA(air.full);else EA(air.bad);
	lx_os_unlock();
	ES(air.decode_busy,0);
}
static bool auto_note_ip(const uint8_t mac[6],const uint8_t *ip,size_t n)
{
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();int i=auto_find_locked(mac,now);
	bool ok=i>=0 && lx_awdl_peer_note_ipv6(&air.table,(unsigned)i,air.table.peers[i].serial,mac,ip,n,now);
	if (ok) EA(air.rv6);
	lx_os_unlock();return ok;
}
static bool auto_active_fresh(void)
{
	if (!air.active) return false;
	lx_os_lock();
	const struct lx_awdl_peer *p=&air.table.peers[air.active_slot];
	bool ok=p->serial==air.active_serial && !memcmp(p->data_mac,active_peer_mac,6) &&
	    !(lx_awdl_peer_expired(&air.table,lx_os_nanoseconds()/1000ull)&(1u<<air.active_slot));
	lx_os_unlock();return ok;
}
static unsigned auto_phy_chan(const struct lx_awdl_peer *p,uint64_t now)
{
	/* Start with a channel where this identity was actually received.
	 * Predictions are used only if that runtime channel is ineligible. */
	unsigned heard=channel_of(p->last_rx_mhz);
	if(auto_channel(air.radio,heard,true)) return heard;
	struct awdl_window window;
	if(awdl_window_at(&p->clock,now,0,&window) && window.channel && auto_channel(air.radio,window.channel,true))
		return window.channel;
	/* The last actual labeled RX frequency is evidence of where this
	 * peer was heard. It does not imply a synchronized future grant. */
	return auto_channel(air.radio,heard,true)?heard:0;
}
static bool auto_pick(void)
{
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();
	int ix=lx_awdl_peer_select(&air.table,air.local,now);
	if (air.wanted_serial && air.wanted<8 && air.table.peers[air.wanted].serial==air.wanted_serial &&
	    !(lx_awdl_peer_expired(&air.table,now)&(1u<<air.wanted))) ix=(int)air.wanted;
	if (ix<0) {lx_os_unlock();return false;}
	const struct lx_awdl_peer *p=&air.table.peers[ix];
	unsigned channel=auto_phy_chan(p,now);
	if (!channel) {lx_os_unlock();return false;}
	air.active_slot=(unsigned)ix;air.active_serial=p->serial;air.channel=channel;air.mhz=auto_freq(channel);
	memcpy(active_peer_mac,p->data_mac,6);memcpy(active_peer_ip,p->has_observed_ipv6?p->observed_ipv6:p->eui64,16);
	air.wanted_serial=0;air.active=1;
	unsigned cls=p->device_class, heard=p->last_rx_mhz, same=p->data_address_matches;
	lx_os_unlock();
	lx_tell("awdl-auto: selected serial=%llu class=%u heard_mhz=%u channel=%u data_equals_mgmt=%u\n",
	    air.active_serial,cls,heard,channel,same);
	return true;
}
static bool auto_lane_target(const uint8_t mac[6])
{
	if (air.active && !memcmp(mac,active_peer_mac,6)) return true;
	int i=auto_find_locked(mac,lx_os_nanoseconds()/1000ull);
	if(i>=0) {air.wanted=(unsigned)i;air.wanted_serial=air.table.peers[i].serial;}
	return false;
}
