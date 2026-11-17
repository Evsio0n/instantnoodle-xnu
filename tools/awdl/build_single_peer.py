"""Rebuild the proven single-peer radio privately, retaining its exact source.

The additional-default-peer experiment asserts this phone's WLAN firmware;
it is not the normal test backend. This builder checks the last working radio
and changes only the bounded native-test lifetime and packet validation log.
"""
import json,subprocess,argparse
from build_roc import BUS,HERE,OUT,read,write,once,sha

BASE='/tmp/awdl-auto-Gedi6C'
BASE_SHA='825ca55b8d510928fafe76049b438842455c6c1abce33c7f8601671b6a7cb5b2'

def replace_output(source,replacement):
    start=source.index('static int live_output(');brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    return source[:start]+replacement+'\n'+source[end:]

def build(raw_follow=False,wire_bssid=False,native_follow=False,firmware_route=False,rx_audit=False,receive_fix=False):
    if receive_fix:rx_audit=True
    if rx_audit:native_follow=True
    if firmware_route:native_follow=True
    if native_follow:wire_bssid=True
    if raw_follow and wire_bssid:raise ValueError('one radio profile at a time')
    old=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',old],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('proven single-peer input changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-single-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    live=read(src+'live_transport.inc')
    live=once(live,'now-live_first_us>=165000000ull','now-live_first_us>=420000000ull')
    live=replace_output(live,(HERE/'auto_output.inc').read_text())
    # Keep a genuine multicast RA for a native IPv6 multicast packet. The
    # firmware's group rate is configured below using its normal rate code.
    live=once(live,'if(f->group) memcpy(frame+4,live.scratch.link_target,6);','')
    live=once(live,'unsigned tag,proto,group,basis,','unsigned tag,proto,group,wire_group,basis,')
    live=once(live,'struct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);\n\tti->control.vif=v;ti->band=ch->band;',
        'f->wire_group=!!(frame[4]&1);\n\tstruct ieee80211_tx_info *ti=IEEE80211_SKB_CB(skb);\n\tti->control.vif=v;ti->band=ch->band;')
    live=once(live,'live.tqm_ack+=!!f->acked;','live.tqm_ack+=!!(f->acked && !f->wire_group);')
    live=once(live,'!!snapshot.acked,snapshot.free_path','!!(snapshot.acked&&!snapshot.wire_group),snapshot.free_path')
    if receive_fix:
        marker='struct ieee80211_tx_control control={.sta=live.peer};'
        probe="""const uint8_t *ip = frame + 40;
        if (bytes >= 60 && ip[6] == 6 && (ip[53] & 2) && !(ip[53] & 16)) {
            uint8_t expected[16];
            lx_peer_make_eui64(expected, frame + 4);
            lx_tell("awdl-syn: ticket=%u src_port=%u dst_port=%u peer_match=%u dst_eui_match=%u checksum=%d channel=%u basis=%u peer_serial=%llu\\n",
                f->tag, (unsigned)ip[40]*256+ip[41], (unsigned)ip[42]*256+ip[43],
                !memcmp(frame+4,active_peer_mac,6), !memcmp(expected,ip+24,16),
                awdl_wire_checksum(ip,bytes), channel_of(ch->center_freq), basis,air.active_serial);
        }
        """
        start=live.index('static void live_data(')
        live=live[:start]+once(live[start:],marker,probe+marker)
        # TX transfers skb ownership. Log the saved group flag after dispatch.
        live=once(live,'!(frame[4]&1),!air.prepeer','!f->wire_group,!air.prepeer')
    write(src+'wire_checksum.h',(HERE/'wire_checksum.h').read_text())
    write(src+'live_transport.inc',live)
    mac_path=work+'src/ath11k/mac.c';mac=read(mac_path)
    anchor='av->aid=vif->cfg.aid; ether_addr_copy(av->bssid,vif->bss_conf.bssid);'
    mac=once(mac,anchor,'''u32 multicast_rate=0;bool found=false;
        for(unsigned i=0;i<ARRAY_SIZE(ath11k_legacy_rates);i++) {
            if(ath11k_legacy_rates[i].bitrate==60) {
                multicast_rate=ATH11K_HW_RATE_CODE(ath11k_legacy_rates[i].hw_value,0,WMI_RATE_PREAMBLE_OFDM);
                found=true;break;
            }
        }
        int mc=found?ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_MCAST_DATA_RATE,multicast_rate):-EINVAL;
        int bc=found?ath11k_wmi_vdev_set_param_cmd(ar,av->vdev_id,WMI_VDEV_PARAM_BCAST_DATA_RATE,multicast_rate):-EINVAL;
        lx_tell("awdl-auto: multicast_rate_kbps=6000 rate_code=%u mcast_rc=%d bcast_rc=%d\\n",multicast_rate,mc,bc);
        '''+anchor)
    if wire_bssid:
        # Keep the sole actual peer and its normal address-search context.
        # Only the VDEV's on-air receive network changes: an AWDL data MPDU
        # has the protocol BSSID, not the selected station's source address.
        # Earlier common-BSSID experiments preceded the working TX carrier;
        # this isolates the receive-network setting on the proven backend.
        call='rc=ath11k_wmi_vdev_up(ar,av->vdev_id,av->aid,av->bssid,NULL,0,0);'
        mac=once(mac,call,'''static const u8 wire_network[6]={0,0x25,0,0xff,0x94,0x73};
        rc=ath11k_wmi_vdev_up(ar,av->vdev_id,av->aid,wire_network,NULL,0,0);
        lx_tell("awdl-auto: up_wire_bssid_is_awdl=1 extra_peer=0 submit_rc=%d\\n",rc);''')
    write(mac_path,mac)
    if raw_follow or native_follow:
        from follow_build import apply
        apply(stage,raw=raw_follow)
    if firmware_route:
        dp_path=work+'src/ath11k/dp_tx.c';dp=read(dp_path)
        # Exercise the driver's existing explicit-peer/TO_FW path (used by
        # normal 4-address multicast) with our SINGLE mapped secondary peer.
        # This is an experimental forwarding choice, not a remote-ACK claim
        # or another WMI peer allocation. Primary descriptors stay unchanged.
        dp=once(dp,'lx_awdl_data_profile(ar,arvif,arsta,&ti);','''if(arsta && lx_awdl_descriptor_role(ar->hw,arvif->vif)==2) {
            ti.meta_data_flags=arsta->tcl_metadata;
            ti.flags0|=FIELD_PREP(HAL_TCL_DATA_CMD_INFO1_TO_FW,1);
            static unsigned announced;
            if(!__atomic_exchange_n(&announced,1,__ATOMIC_RELAXED))
                lx_tell("awdl-auto: firmware_peer_route=1 additional_peer=0 secondary_only=1\\n");
        }
        lx_awdl_data_profile(ar,arvif,arsta,&ti);''')
        write(dp_path,dp)
    if rx_audit:
        from rx_boundary_build import apply as apply_rx_audit
        apply_rx_audit(stage)
    if receive_fix:
        owner_path=src+'auto_owner.inc';owner=read(owner_path)
        # The persistent owner clears echo when replacing its carrier. The
        # old live_service armed RX, but auto_data_service replaced that
        # function without retaining the arm. Restore that lifecycle step
        # ONLY after the existing successful UP/authorize caller enters it.
        # The enclosing carrier already disarms RX before DOWN/peer teardown.
        owner=once(owner,'live.vif=v;live.peer=peer;','''live.vif=v;live.peer=peer;
        ES(echo.receive,1);
        lx_tell("awdl-auto: native_receive_arm=%u hardware_owner_matches=%u\\n",EG(echo.receive),echo.hw==&r->hw);''')
        write(owner_path,owner)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_mac.o'],check=True)
    if raw_follow or firmware_route:subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_dp_tx.o'],check=True)
    if raw_follow:subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_wmi.o'],check=True)
    with (OUT/'single-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    profile='native-receive-fix' if receive_fix else 'rx-boundary-audit' if rx_audit else 'firmware-peer-route' if firmware_route else 'raw-follow' if raw_follow else 'native-follow' if native_follow else 'wire-bssid' if wire_bssid else 'single-peer'
    dest=OUT/('ath11k-'+profile+'.o')
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    receipt='native-receive-fix-build.json' if receive_fix else 'rx-boundary-audit-build.json' if rx_audit else 'firmware-peer-route-build.json' if firmware_route else 'raw-follow-build.json' if raw_follow else 'native-follow-build.json' if native_follow else 'wire-bssid-build.json' if wire_bssid else 'single-build.json'
    (OUT/receipt).write_text(json.dumps(dict(object=str(dest),sha256=sha(dest),stage=stage,
        source_radio_sha256=BASE_SHA,additional_peer=False,fixed_endpoint=False,lifetime_seconds=420,
        multicast_rate_kbps=6000,multicast_link_destination='original',raw_follow=raw_follow,wire_bssid=wire_bssid,native_follow=native_follow,firmware_peer_route=firmware_route,rx_boundary_audit=rx_audit,native_receive_fix=receive_fix),indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);profiles=parser.add_mutually_exclusive_group()
    profiles.add_argument('--raw-follow',action='store_true');profiles.add_argument('--wire-bssid',action='store_true')
    profiles.add_argument('--native-follow',action='store_true')
    profiles.add_argument('--firmware-peer-route',action='store_true')
    profiles.add_argument('--rx-boundary-audit',action='store_true')
    profiles.add_argument('--native-receive-fix',action='store_true')
    args=parser.parse_args();build(args.raw_follow,args.wire_bssid,args.native_follow,args.firmware_peer_route,args.rx_boundary_audit,args.native_receive_fix)
