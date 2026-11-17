/* Wired Ethernet semantics after native 802.11 deaggregation or 802.3 decap.
 * Return true only for an allowed peer/local IPv6 packet we consumed. */
static bool lx_awdl_nd_l2(const uint8_t *da)
{
	static const uint8_t all_nodes[6]={0x33,0x33,0,0,0,1};
	return !memcmp(da,echo.mac,6) || !memcmp(da,all_nodes,6) ||
	    (da[0]==0x33 && da[1]==0x33 && da[2]==0xff && !memcmp(da+3,echo.ip+13,3));
}
static void lx_awdl_checked_format(unsigned format)
{
	if (format==2) EA(echo.nd_eth_short);
	else if (format==1) EA(echo.nd_eth_ip);
	else EA(echo.nd_native);
}
static bool lx_awdl_nd_input(const uint8_t *da,const uint8_t *ip,size_t bytes,unsigned format)
{
	if (!lx_awdl_valid_ipv6(ip,bytes)) return false;
	bool unicast=!memcmp(da,echo.mac,6);
	bool group=da[0]==0x33 && da[1]==0x33 && ip[24]==0xff && !memcmp(da+2,ip+36,4);
	if ((!unicast && !group) || (unicast && memcmp(ip+24,echo.ip,16))) return false;
	if (EG(echo.nd_watch)) {
		unsigned nd=lx_awdl_nd6_observe(ip,bytes,echo.ip,awdl_target_ip,awdl_target_mac);
		if (nd==1 || nd==2) {
			lx_awdl_checked_format(format);
			if (nd==1) { if (group) EA(echo.nd_ns_group); else EA(echo.nd_ns_uni); }
			else EA(echo.nd_na_local);
			if (!EG(echo.submitted)) EA(echo.nd_before_tx);
			return true;
		}
	}
	if (!unicast || !EG(echo.receive) || memcmp(ip+8,awdl_target_ip,16)) return false;
	EA(echo.received);
	if (hk_awdl_echo6_reply(ip,bytes,echo.ip,awdl_target_ip,echo.nonce)) {
		lx_awdl_checked_format(format); ES(echo.matched,1);
	}
	return true;
}
