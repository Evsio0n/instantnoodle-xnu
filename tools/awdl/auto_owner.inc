/* One generation owns discovery, its real netif and a replaceable data peer.
 * No address is taken from a host file. The RX table is populated by complete
 * radio management frames; native TLS still decides application identity. */
#include "auto_direct.inc"
static void auto_schedule_queued(void)
{
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();
	struct live_fifo *q=live.tx.count?&live.tx:&live.groups;
	for(unsigned attempts=0;q->count && attempts<LIVE_DEPTH;attempts++) {
		struct live_frame *f=&q->slot[q->first];
		/* Finish this peer's pending group copies together. Native unicast
		 * has priority and retains its current carrier during a transfer. */
		if(q==&live.groups && air.active && auto_find_locked(active_peer_mac,now)>=0) {
			bool same=false;
			for(unsigned i=0;i<q->count;i++)
				if(!memcmp(q->slot[(q->first+i)%LIVE_DEPTH].link_target,active_peer_mac,6))same=true;
			if(same || now<air.unicast_until_us) {lx_os_unlock();return;}
		}
		const uint8_t *target=f->link_target;
		int i=lx_peer_unicast(target)?auto_find_locked(target,now):-1;
		if(!lx_peer_unicast(target) && q==&live.groups) {
			for(unsigned k=0;k<LX_AWDL_PEER_SLOTS;k++) {
				struct lx_awdl_peer *p=&air.table.peers[k];
				if(p->serial && p->seen_mif && p->version && p->device_class &&
				    !(lx_awdl_peer_expired(&air.table,now)&(1u<<k))) {i=(int)k;memcpy(f->link_target,p->data_mac,6);break;}
			}
			if(i<0) {lx_os_unlock();return;}
		}
		if(i>=0) {
			if(!air.active || air.active_serial!=air.table.peers[i].serial || memcmp(active_peer_mac,target,6)) {
				air.wanted=(unsigned)i;air.wanted_serial=air.table.peers[i].serial;
			}
			lx_os_unlock();return;
		}
		/* A queued target expired or changed its advertised data address.
		 * No synthetic peer is created to keep that old route alive. */
		live_pop(q);live.dropped++;
	}
	lx_os_unlock();
}

static bool auto_window(struct lx_radio *r,struct ieee80211_vif *v,unsigned channel)
{
	struct linuxkpi_ieee80211_channel *ch=linuxkpi_ieee80211_get_channel(r->wiphy,auto_freq(channel));
	if (!ch || !auto_channel(r,channel,false)) return true;
	if (lx_awdl_scan_state(&r->hw,EG(r->roc.request_seq),echo.vdev)!=LX_AWDL_SCAN_IDLE) {
		live_retire();live_deliver();lx_pause_ms(25);return true;
	}
	if (r->channel->def.chan->center_freq==ch->center_freq) {
		/* The existing associated receiver already delivers this channel. */
		live_retire();live_deliver();lx_pause_ms(25);return true;
	}
	memset(&bw,0,sizeof(bw));bw.radio=r;bw.vif=r->vif;
	bw.control_id=lx_awdl_roc_vdev(bw.vif);bw.own_mhz=ch->center_freq;
	ES(bw.armed,1);air.listen_mhz=ch->center_freq;live_infra_window=1;
	int error=lx_awdl_dispatch_roc(r);
	if (error) {ES(bw.armed,0);return error==EBUSY;}
	uint64_t deadline=lx_os_nanoseconds()+8500000000ull;
	while (!EG(bw.finished) && lx_os_nanoseconds()<deadline) {
		live_retire();live_deliver();auto_maintain();
		if (lx_awdl_data_foreign(&r->hw) && channel==6 && auto_channel(r,6,true)) live_discovery(r,v,ch);
		lx_pause_ms(2);
	}
	ES(bw.armed,0);
	return EG(bw.finished) && lx_awdl_scan_state(&r->hw,EG(r->roc.request_seq),bw.control_id)!=LX_AWDL_SCAN_OURS;
}

static bool auto_data_service(struct lx_radio *r,struct ieee80211_vif *v,struct ieee80211_sta *peer,
    struct linuxkpi_ieee80211_channel *ch)
{
	live.vif=v;live.peer=peer;
	if(air.active && !air.prepeer) {
		int direct_rc=auto_direct_open(r,v,peer,ch);
		if(direct_rc) {ES(live.stop,1);return direct_rc!=-EBUSY;}
	}
	lx_tell("awdl-auto: data_active=1 prepeer_group_context=%u peer_serial=%llu channel=%u\n",air.prepeer,air.active_serial,air.channel);
	unsigned cycles=0;
	while (!EG(live.stop) && !live_deadline() && (air.prepeer || auto_active_fresh())) {
		live_retire();live_deliver();auto_maintain();
		auto_schedule_queued();
		if(air.prepeer) {
			lx_os_lock();int found=lx_awdl_peer_select(&air.table,air.local,lx_os_nanoseconds()/1000ull);lx_os_unlock();
			if(found>=0 || air.wanted_serial) break;
		}
		lx_os_lock();bool change=air.wanted_serial && air.wanted_serial!=air.active_serial;lx_os_unlock();
		if(change) break;
		int state=lx_awdl_scan_state(&r->hw,EG(r->roc.request_seq),echo.vdev);
		if(state!=LX_AWDL_SCAN_IDLE) {lx_pause_ms(25);continue;}
		if(!air.prepeer && ch->center_freq!=2437 && (++cycles%3)==1) {
			if(!auto_window(r,v,6)) {ES(live.stop,1);break;}
			continue;
		}
		air.listen_mhz=ch->center_freq;
		if(r->channel && r->channel->def.chan && r->channel->def.chan->center_freq==ch->center_freq) {
			live_tick(r,v,ch,lx_awdl_home_basis(&r->hw));lx_pause_ms(2);continue;
		}
		if(air.prepeer && cycles && cycles%3) {
			if(!auto_window(r,v,(cycles++%3)==1?44:149)) {ES(live.stop,1);break;}
			continue;
		}
		live_infra_window=(++cycles%4)!=0;
		memset(&bw,0,sizeof(bw));
		if(lx_awdl_station_window(r,v,ch)<0) {ES(live.stop,1);break;}
		if(!EG(live.ready) && EG(live_wmi_psf) && EG(live_wmi_mif)) {
			ES(live.ready,1);int e=lx_os_awdl_set_ready(live.generation,true);if(e)ES(live.ready,0);
			lx_tell("awdl-auto: ready rc=%d prepeer=%u psf_fw=%u mif_fw=%u\n",e,air.prepeer,EG(live_wmi_psf),EG(live_wmi_mif));
		}
		for(unsigned i=0;i<4 && !EG(live.stop);i++) {live_retire();live_deliver();auto_maintain();lx_pause_ms(5);}
	}
	/* Finish callbacks for this hardware carrier before its peer is retired.
 * The OS interface and its generation remain live across that replacement. */
	uint64_t end=lx_os_nanoseconds()+1200000000ull;
	while(EG(live.mgmt_pending) && lx_os_nanoseconds()<end) {live_retire();lx_pause_ms(2);}
	lx_tell("awdl-auto: data_retiring=1 peer_serial=%llu expired=%u queued=%u flights=%u\n",
	    air.active_serial,!auto_active_fresh(),live.tx.count+live.groups.count,live_count_flights(0));
	bool direct_clean=auto_direct_close(r,v);live.peer=peer;
	return !EG(live.mgmt_pending) && direct_clean;
}

#include "auto_maintenance.inc"

static bool auto_carrier_delete(struct lx_radio *r,struct ieee80211_vif *v)
{
	struct lx_awdl_roc_state *s=&r->roc;
	ES(s->visible,NULL);
	uint64_t end=lx_os_nanoseconds()+1500000000ull;
	while(EG(s->iterating) && lx_os_nanoseconds()<end) lx_pause_ms(2);
	if(EG(s->iterating) || !lx_awdl_clear_secondary(r,s->p2p_id)) return false;
	ES(s->delete_events,0);ES(s->deleting,1);
	mutex_lock(&r->wiphy->mtx);r->ops->remove_interface(&r->hw,v);mutex_unlock(&r->wiphy->mtx);
	ES(s->deleting,0);
	return EG(s->delete_events)==1;
}

static void auto_owner(struct lx_radio *r)
{
	struct ieee80211_hw *hw=&r->hw;struct wiphy *w=hw->wiphy;
	if(!r->joined || !w->addresses || w->n_addresses<2 || !r->ops->add_interface || !r->ops->remove_interface) {
		lx_tell("awdl-auto: unavailable joined=%u addresses=%u\n",r->joined,w->n_addresses);return;
	}
	const uint8_t *address=NULL;
	for(unsigned i=1;i<w->n_addresses;i++) {
		const uint8_t *candidate=w->addresses[i].addr;
		if(lx_peer_unicast(candidate) && memcmp(candidate,r->vif->addr,6)) {address=candidate;break;}
	}
	if(!address) {lx_tell("awdl-auto: no distinct driver address\n");return;}
	void *storage=kzalloc(sizeof(struct ieee80211_vif)+hw->vif_data_size+64,GFP_KERNEL);
	if(!storage) return;
	struct ieee80211_vif *v=(void *)roundup2((uintptr_t)storage,64);
	v->type=NL80211_IFTYPE_STATION;v->p2p=true;memcpy(v->addr,address,6);memcpy(v->bss_conf.addr,address,6);
	v->bss_conf.vif=v;v->bss_conf.bssid=no_network;v->link_conf[0]=&v->bss_conf;
	uint64_t before=lx_awdl_allocated_vdevs(hw);
	mutex_lock(&w->mtx);int rc=r->ops->add_interface(hw,v);mutex_unlock(&w->mtx);
	unsigned id=lx_awdl_roc_vdev(v);
	if(rc) {lx_tell("awdl-auto: add_interface rc=%d\n",rc);kfree(storage);return;}
	if(id>=64 || lx_awdl_allocated_vdevs(hw)!=(before|(1ull<<id)) || (before&(1ull<<id))) {
		lx_tell("awdl-auto: allocation did not match secondary VDEV\n");return;
	}
	r->roc.p2p_id=id;ES(r->roc.visible,v);
	memset(&echo,0,sizeof(echo));echo.hw=hw;echo.vdev=id;memcpy(echo.mac,address,6);lx_peer_make_eui64(echo.ip,address);
	ES(echo.active,1);ES(echo.receive,1);air.radio=r;air.vif=v;memcpy(air.local,address,6);ES(air.running,1);
	live.radio=r;live.vif=v;live.anchor_us=lx_os_nanoseconds()/1000ull;live_first_us=live.anchor_us;ES(live.armed,1);
	rc=lx_os_awdl_register(&live_ops,&live,&live.generation);
	if(!rc) ES(live.registered,1);else ES(live.stop,1);
	int rx_mode=EG(live.stop)?-ECANCELED:lx_awdl_receiver_mode(hw,1);
	lx_tell("awdl-auto: receiver_promisc_submit_rc=%d advertised_service_required=1\n",rx_mode);
	lx_tell("awdl-auto: started rc=%d fixed_targets=0 vdev=%u mtu=%u\n",rc,id,LIVE_MTU);
	static const unsigned social[]={6,44,149};unsigned cycle=0;
	while(!EG(live.stop) && !live_deadline()) {
		auto_maintain();live_retire();live_deliver();
		auto_schedule_queued();
		if(auto_pick()) {
			/* Selection came from a live radio record, never an endpoint file. */
			ES(echo.active,0);memset(&echo,0,sizeof(echo));
			if(!lx_awdl_echo_trial(r,v)) {ES(live.stop,1);break;}
			air.active=0;EA(air.switched);
			echo.hw=hw;echo.vdev=id;memcpy(echo.mac,address,6);lx_peer_make_eui64(echo.ip,address);
			ES(echo.active,1);ES(echo.receive,1);live.peer=NULL;
		} else if(auto_channel(r,6,true)) {
			/* A local open BSS/rate context uses the protocol's common
			 * AWDL BSSID. This is not a discovered remote peer. It lets
			 * native multicast leave before any unicast address exists. */
			air.prepeer=1;air.channel=6;air.mhz=2437;
			memcpy(active_peer_mac,awdl_network,6);memset(active_peer_ip,0,16);
			ES(echo.active,0);memset(&echo,0,sizeof(echo));
			bool ok=lx_awdl_echo_trial(r,v);
			air.prepeer=0;live.peer=NULL;
			echo.hw=hw;echo.vdev=id;memcpy(echo.mac,address,6);lx_peer_make_eui64(echo.ip,address);
			ES(echo.active,1);ES(echo.receive,1);
			if(!ok) {ES(live.stop,1);break;}
		} else {
			unsigned channel=social[cycle++%3];
			if(!auto_window(r,v,channel)) {ES(live.stop,1);break;}
		}
		if(!EG(live.ready) && EG(live_wmi_psf) && EG(live_wmi_mif)) {
			ES(live.ready,1);rc=lx_os_awdl_set_ready(live.generation,true);if(rc)ES(live.ready,0);
			lx_tell("awdl-auto: ready rc=%d peers_learned=%u fixed_targets=0\n",rc,EG(air.psf)+EG(air.mif));
		}
		if(!(cycle%8)) lx_tell("awdl-auto: cycle=%u psf_rx=%u mif_rx=%u invalid=%u expired=%u switched=%u lease=%u native_rx=%u tx_ack=%u\n",
			cycle,EG(air.psf),EG(air.mif),EG(air.bad),EG(air.expired),EG(air.switched),!!EG(live.lease),EG(live.delivered),EG(live.tqm_ack));
	}
	ES(live.ready,0);(void)lx_os_awdl_set_ready(live.generation,false);
	ES(live.accept,0);lx_awdl_end_roc_worker();
	uint64_t end=lx_os_nanoseconds()+1500000000ull;
	while((EG(live.mgmt_pending)||live_count_flights(0)) && lx_os_nanoseconds()<end) {live_retire();lx_pause_ms(2);}
	live_withdraw();ES(air.running,0);ES(live.armed,0);ES(echo.receive,0);
	if(!rx_mode)lx_tell("awdl-auto: receiver_promisc_restore_rc=%d\n",lx_awdl_receiver_mode(hw,0));
	bool clean=!direct.storage && !EG(live.registered) && !EG(live.mgmt_pending) && !live_count_flights(0) && auto_carrier_delete(r,v) && lx_awdl_allocated_vdevs(hw)==before;
	lx_tell("awdl-auto: retired clean=%u fixed_targets=0\n",clean);
	if(clean)kfree(storage);
}
