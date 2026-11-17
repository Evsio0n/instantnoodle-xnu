"""Add passive receive-stage observations, retaining all existing predicates."""
from build_roc import read,write,once,HERE

def apply(stage):
    src=stage+'/wifi/lx/src/'
    write(src+'rx_boundary_audit.h',(HERE/'rx_boundary_audit.h').read_text())
    data=read(src+'data.inc')
    data=once(data,'#include "auto_peers.inc"','#include "auto_peers.inc"\n#include "rx_boundary_audit.h"')
    start=data.index('static bool lx_awdl_echo_receive(')
    end=data.index('\nstatic struct ieee80211_vif *lookup_vif;',start)
    rx=data[start:end]
    rx=once(rx,'size_t n=skb_headlen(skb);','size_t n=skb_headlen(skb);\n\tbool audit_data=rx_boundary_begin(p,n,!!(s->flag&RX_FLAG_8023));')
    # Preserve each original predicate, its order, and its false result.
    old='(s->flag&(RX_FLAG_MMIC_ERROR|RX_FLAG_FAILED_FCS_CRC|RX_FLAG_FAILED_PLCP_CRC))) return false;'
    rx=once(rx,old,old.replace('return false','return rx_boundary_drop(RXA_EARLY,audit_data)'))
    old='if (n<14 || !auto_route(p+6,NULL) || (!EG(live.armed) && !lx_awdl_nd_l2(p))) return false;'
    rx=once(rx,old,old.replace('return false','return rx_boundary_drop(RXA_ETH_ADMISSION,audit_data)'))
    rx=once(rx,'else return false;','else return rx_boundary_drop(RXA_ETH_SHAPE,audit_data);')
    old='memcmp(p+16,awdl_network,6)) return false;'
    rx=once(rx,old,old.replace('return false','return rx_boundary_drop(RXA_RAW_ADMISSION,audit_data)'))
    rx=once(rx,'if (n<h) return false;','if (n<h) return rx_boundary_drop(RXA_SHORT,audit_data);')
    rx=once(rx,'{ EA(echo.nd_amsdu); return false; }','{ EA(echo.nd_amsdu); return rx_boundary_drop(RXA_AMSDU,audit_data); }')
    old='if (n<h || !lx_awdl_ipv6_decap(p+h,n-h,&ipv6)) return false;'
    rx=once(rx,old,old.replace('return false','return rx_boundary_drop(RXA_DECAP,audit_data)'))
    old='return lx_awdl_nd_input((s->flag&RX_FLAG_8023)?p+6:p+10,da,ip,bytes,format);'
    rx=once(rx,old,'''RXA_INC(rx_boundary.parsed);
    bool accepted=lx_awdl_nd_input((s->flag&RX_FLAG_8023)?p+6:p+10,da,ip,bytes,format);
    if(!accepted)RXA_INC(rx_boundary.post_rejected);
    return accepted;''')
    write(src+'data.inc',data[:start]+rx+data[end:])
    owner=read(src+'auto_owner.inc')
    marker='static void auto_owner(struct lx_radio *r)'
    report='''static void rx_boundary_report(void)
{
    lx_tell("awdl-rx-boundary: host_eth=%u host_raw_data=%u raw_awdl=%u raw_awdl_bad_header=%u vendor_eth=%u length_vendor_eth=%u drop_early=%u drop_eth_admission=%u drop_eth_shape=%u drop_raw_admission=%u drop_short=%u drop_amsdu=%u drop_decap=%u parsed=%u post_rejected=%u ipv6_observed=%u native_queued=%u ifnet_errors=%u native_delivered=%u\\n",
        EG(rx_boundary.host_eth),EG(rx_boundary.host_raw_data),EG(rx_boundary.raw_awdl),EG(rx_boundary.raw_awdl_bad_header),
        EG(rx_boundary.vendor_eth),EG(rx_boundary.length_vendor_eth),EG(rx_boundary.drop[RXA_EARLY]),
        EG(rx_boundary.drop[RXA_ETH_ADMISSION]),EG(rx_boundary.drop[RXA_ETH_SHAPE]),EG(rx_boundary.drop[RXA_RAW_ADMISSION]),
        EG(rx_boundary.drop[RXA_SHORT]),EG(rx_boundary.drop[RXA_AMSDU]),EG(rx_boundary.drop[RXA_DECAP]),
        EG(rx_boundary.parsed),EG(rx_boundary.post_rejected),EG(air.rv6),EG(live.rx_observed),EG(live.failed_input),EG(live.delivered));
}
'''
    owner=once(owner,marker,report+'\n'+marker)
    owner=once(owner,'if(!(cycle%8)) lx_tell(', 'rx_boundary_report();\n\t\tif(!(cycle%8)) lx_tell(')
    owner=once(owner,'live_withdraw();ES(air.running,0);','rx_boundary_report();live_withdraw();ES(air.running,0);')
    write(src+'auto_owner.inc',owner)
