static bool auto_other_claim(unsigned slot,const uint8_t mac[6],const uint8_t *ip,uint64_t now)
{
	lx_os_lock();bool result=lx_awdl_other_claim(&air.table,slot,mac,ip,now);lx_os_unlock();return result;
}
static int auto_absent_ack(uint64_t generation,uint64_t lease,unsigned source,const uint8_t mac[6],const uint8_t *ip)
{
	int error=lx_os_awdl_peer_absent(generation,lease,source,mac,ip);
	/* The kernel validates this exact lease before returning ENOENT for
	 * a route that is already gone. Desired absence is then satisfied. */
	return error==ENOENT?0:error;
}
static void auto_maintain(void)
{
	uint64_t now=lx_os_nanoseconds()/1000ull;
	if(now-air.last_maint_us<20000) return;
	air.last_maint_us=now;
	for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
		struct lx_awdl_peer p;
		lx_os_lock();p=air.table.peers[i];uint64_t lease=live.lease,g=live.generation;
		bool gone=p.serial && (lx_awdl_peer_expired(&air.table,now)&(1u<<i));lx_os_unlock();
		if(!p.serial) continue;
		if(air.notice[i].serial!=p.serial || air.lease_seen!=lease) {
			memset(&air.notice[i],0,sizeof(air.notice[i]));air.notice[i].serial=p.serial;
			memcpy(air.notice[i].mac,p.data_mac,6);
		}
		int error=0;
		if(lease && memcmp(air.notice[i].mac,p.data_mac,6)) {
			if(air.notice[i].ipv6) {
				if(!auto_other_claim(i,air.notice[i].mac,air.notice[i].ip,now)) error=auto_absent_ack(g,lease,HK_AWDL_PEER_OVER_AIR_VERIFIED,air.notice[i].mac,air.notice[i].ip);
				if(!error)air.notice[i].ipv6=0;
			}
			if(!error && air.notice[i].l2) {
				if(!auto_other_claim(i,air.notice[i].mac,NULL,now)) error=auto_absent_ack(g,lease,HK_AWDL_PEER_OVER_AIR_L2,air.notice[i].mac,NULL);
				if(!error)air.notice[i].l2=0;
			}
			if(error) continue;
			air.notice[i].l2=air.notice[i].ipv6=0;memcpy(air.notice[i].mac,p.data_mac,6);
		}
		if(gone) {
			if(lease && air.notice[i].ipv6) {
				if(!auto_other_claim(i,air.notice[i].mac,air.notice[i].ip,now)) error=auto_absent_ack(g,lease,HK_AWDL_PEER_OVER_AIR_VERIFIED,air.notice[i].mac,air.notice[i].ip);
				if(!error)air.notice[i].ipv6=0;
			}
			if(!error && lease && air.notice[i].l2) {
				if(!auto_other_claim(i,air.notice[i].mac,NULL,now)) error=auto_absent_ack(g,lease,HK_AWDL_PEER_OVER_AIR_L2,air.notice[i].mac,NULL);
				if(!error)air.notice[i].l2=0;
			}
			if(!error && (!air.active || air.active_serial!=p.serial)) {
				lx_os_lock();bool removed=lx_awdl_peer_remove(&air.table,i,p.serial);lx_os_unlock();
				if(removed) {memset(&air.notice[i],0,sizeof(air.notice[i]));EA(air.expired);lx_tell("awdl-auto: peer_expired serial=%llu\n",p.serial);}
			}
		} else if(lease && EG(live.accept)) {
			if(!air.notice[i].l2) {error=lx_os_awdl_peer_present(g,lease,HK_AWDL_PEER_OVER_AIR_L2,p.data_mac,NULL);if(!error)air.notice[i].l2=1;}
			if(!error && p.has_observed_ipv6 && (!air.notice[i].ipv6 || memcmp(air.notice[i].ip,p.observed_ipv6,16))) {
				error=lx_os_awdl_peer_present(g,lease,HK_AWDL_PEER_OVER_AIR_VERIFIED,p.data_mac,p.observed_ipv6);
				if(!error) {air.notice[i].ipv6=1;memcpy(air.notice[i].ip,p.observed_ipv6,16);}
			}
		}
	}
	air.lease_seen=live.lease;
}

