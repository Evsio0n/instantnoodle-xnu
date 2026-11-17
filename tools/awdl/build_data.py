"""Build the single controlled IPv6 data trial from the preserved #420 source."""
import json
import os
from pathlib import Path
import subprocess
from build_roc import BUS, HERE, ROOT, OUT, once, read, sha, write

PRIOR = '1e395077a95fafc10a667cef216eaa3ba863660c8b1b9df3feac8aafda7219cd'
# The two tokens are replaced only in the linked Mach-O, never in C or logs.
MAC_TOKEN = b'AWDLM!'
IP_TOKEN = b'AWDLprivateIP___'
DECL = '''struct ieee80211_chanctx_conf;
int lx_awdl_echo_ctx(struct ieee80211_hw *,struct ieee80211_chanctx_conf *);
void lx_awdl_echo_peer(unsigned,const unsigned char *,int);
void lx_awdl_echo_vdev(struct ieee80211_hw *,unsigned,int,unsigned);
void lx_awdl_echo_release(struct ieee80211_hw *,struct sk_buff *,int);
void lx_awdl_echo_desc(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned,unsigned,int);
void lx_awdl_echo_wbm(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned,unsigned,unsigned,unsigned,int);
int lx_awdl_echo_private(void);
'''

def build():
    pin=json.loads((OUT/'psf-build.json').read_text())
    if pin['sha256']!=PRIOR or sha(Path(pin['object']))!=PRIOR:
        raise ValueError('expected the tested PSF driver')
    root=ROOT/'out/airdrop'
    mac=(root/'controlled-mac-address.bin').read_bytes()
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    if len(mac)!=6 or mac[0]&1 or not any(mac) or len(ip)!=16 or ip[:2]!=bytes.fromhex('fe80') or any(ip[2:8]):
        raise ValueError('invalid unicast Mac or wire-scope link-local endpoint')
    endpoint_source='supplied-link-address'
    if mac==bytes((2,0,0,0,0,0)):
        if ip[11:13]!=bytes((0xff,0xfe)):
            raise ValueError('masked link address and no modified EUI-64 on current link-local')
        mac=bytes((ip[8]^2,))+ip[9:11]+ip[13:]
        if mac[0]&1 or not any(mac) or mac==bytes((2,0,0,0,0,0)):
            raise ValueError('link-local does not yield a usable EUI-48 candidate')
        endpoint_source='derived-from-current-awdl0-modified-EUI64'
    mark=OUT/'data-stage.txt'
    if mark.exists():
        job=mark.read_text().strip()
        if not job.startswith('/tmp/awdl-data-') or '/' in job.removeprefix('/tmp/awdl-data-'):
            raise ValueError('invalid retained private staging path')
    else:
        job=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-data-XXXXXX'],text=True).strip()
        mark.write_text(job+'\n')
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',job+'/'],check=True)
    src=job+'/wifi/lx/src/'; work=job+'/xnu-deps/build/ath11k/'; vendor=work+'src/ath11k/'
    api=once(read(src+'roc_state.h'),'#endif',DECL+'#endif')
    write(src+'roc_state.h',api); write(vendor+'roc_state.h',api)
    write(src+'echo_target_private.h', '''
extern const unsigned char awdl_target_mac[6],awdl_target_ip[16];
__asm__(".section __DATA,__data\\n"
        "_awdl_target_mac: .ascii \\"AWDLM!\\"\\n"
        "_awdl_target_ip: .ascii \\"AWDLprivateIP___\\"\\n"
        ".text\\n");
''')
    for name,path in [('lx_awdl_data.h',ROOT/'wifi/lx/src/lx_awdl_data.h'),('echo6.h',HERE/'echo6.h'),('data.inc',HERE/'data.inc')]:
        write(src+name,path.read_text())
    lx=read(src+'lx_80211.c')
    lx=once(lx,'radio->roc.choice==47 ||','radio->roc.choice==47 || radio->roc.choice==48 ||')
    lx=once(lx,'r->choice==46 || r->choice==47)', 'r->choice==46 || r->choice==47 || r->choice==48)')
    lx=once(lx,'#include "roc.inc"', '#include "data.inc"\n#include "roc.inc"')
    lx=once(lx,'if (radio->awdl_observe) lx_awdl_roc_rx(radio,skb,status);',
            'if (radio->roc.choice==48 && lx_awdl_echo_receive(radio,skb,status)) { dev_kfree_skb_any(skb); return; }\n\tif (radio->awdl_observe) lx_awdl_roc_rx(radio,skb,status);')
    lx=once(lx,'dev_kfree_skb_any(status->skb);','lx_awdl_echo_release(hw,status->skb,0);\n\t\tdev_kfree_skb_any(status->skb);')
    for i in (0,1):
        old=f'lx_awdl_psf_release(hw,skb,{i});'
        lx=once(lx,old,old+f'\n\tlx_awdl_echo_release(hw,skb,{i});')
    write(src+'lx_80211.c',lx)
    roc=read(src+'roc.inc')
    for old,new in [
      ('mode!=46 && mode!=47','mode!=46 && mode!=47 && mode!=48'),
      ('r->choice!=47 ||','(r->choice!=47 && r->choice!=48) ||'),
      ('vif->type!=NL80211_IFTYPE_P2P_DEVICE ||','(vif->type!=NL80211_IFTYPE_P2P_DEVICE && !(r->choice==48 && vif->type==NL80211_IFTYPE_STATION && vif->p2p)) ||'),
      ('if (!rc && r->choice==47)', 'if (!rc && (r->choice==47 || r->choice==48))'),
      ('if (mode==47 && c!=6)', 'if ((mode==47 || mode==48) && c!=6)'),
      ('if (mode==47 && !lx_awdl_psf_quiesced', 'if ((mode==47 || mode==48) && !lx_awdl_psf_quiesced'),
      ('BIT(NL80211_IFTYPE_P2P_DEVICE)', 'BIT(mode==48?NL80211_IFTYPE_P2P_CLIENT:NL80211_IFTYPE_P2P_DEVICE)'),
      ('peer_vif->type=NL80211_IFTYPE_P2P_DEVICE;', 'peer_vif->type=mode==48?NL80211_IFTYPE_STATION:NL80211_IFTYPE_P2P_DEVICE;'),
      ('peer_vif->bss_conf.vif=peer_vif;', 'memcpy(peer_vif->bss_conf.addr,address,ETH_ALEN);\n\tpeer_vif->bss_conf.vif=peer_vif;'),
      ('rc,sta_id,NL80211_IFTYPE_P2P_DEVICE,id,distinct','rc,sta_id,mode==48?NL80211_IFTYPE_P2P_CLIENT:NL80211_IFTYPE_P2P_DEVICE,id,distinct'),
      ('\tunsigned long long limit=lx_os_nanoseconds()+1500000000ull;','\tunsigned long long limit=lx_os_nanoseconds()+1500000000ull;')]:
        if old not in roc: raise ValueError(old)
        roc=roc.replace(old,new)
    needle='\t__atomic_store_n(&r->visible,NULL,__ATOMIC_SEQ_CST);\n\tunsigned long long limit='
    roc=once(roc,needle,'''	if (mode==48 && distinct && lx_awdl_roc_driver_idle(hw) && !lx_awdl_echo_trial(radio,peer_vif)) return;
'''+needle)
    write(src+'roc.inc',roc)
    macsrc=read(vendor+'mac.c')
    macsrc=once(macsrc,'static int ath11k_mac_op_add_chanctx(', '#include "roc_state.h"\nstatic int ath11k_mac_op_add_chanctx(')
    # The driver's singleton rx_channel is only an RX-report fallback. The
    # secondary VDEV still receives its own real WMI center/chanctx. Keep the
    # existing primary fallback when adding/removing that dedicated context.
    macsrc=once(macsrc,'ar->rx_channel = ctx->def.chan;','if (!lx_awdl_echo_ctx(hw,ctx)) ar->rx_channel = ctx->def.chan;')
    start=macsrc.index('static void ath11k_mac_op_remove_chanctx(')
    macsrc=macsrc[:start]+macsrc[start:].replace('ar->rx_channel = NULL;','if (!lx_awdl_echo_ctx(hw,ctx)) ar->rx_channel = NULL;',1)
    write(vendor+'mac.c',macsrc)
    peer=read(vendor+'peer.c')
    peer=once(peer,'void ath11k_peer_unmap_event(', '#include "roc_state.h"\nvoid ath11k_peer_unmap_event(')
    peer=once(peer,'list_del(&peer->list);\n\tkfree(peer);\n\twake_up(&ab->peer_mapping_wq);','lx_awdl_echo_peer(peer->vdev_id,peer->addr,0);\n\tlist_del(&peer->list);\n\tkfree(peer);\n\twake_up(&ab->peer_mapping_wq);')
    peer=once(peer,'list_add(&peer->list, &ab->peers);','list_add(&peer->list, &ab->peers);\n\t\tlx_awdl_echo_peer(vdev_id,mac_addr,1);')
    write(vendor+'peer.c',peer)
    wmi=read(vendor+'wmi.c')
    wmi=once(wmi,'status = vdev_start_resp.status;','status = vdev_start_resp.status;\n\tlx_awdl_echo_vdev(ar->hw,vdev_start_resp.vdev_id,1,status);')
    start=wmi.index('static void ath11k_vdev_stopped_event(')
    wmi=wmi[:start]+wmi[start:].replace('complete(&ar->vdev_setup_done);','lx_awdl_echo_vdev(ar->hw,vdev_id,0,0);\n\tcomplete(&ar->vdev_setup_done);',1)
    write(vendor+'wmi.c',wmi)
    dp=read(vendor+'dp_tx.c')
    dp=once(dp,'#include "dp_tx.h"','#include "dp_tx.h"\n#include "roc_state.h"')
    dp=once(dp,'ath11k_hal_tx_cmd_desc_setup(ab, hal_tcl_desc +', 'lx_awdl_echo_desc(ar->hw,skb,arvif->vdev_id,ti.ring_id,ti.desc_id,ti.encap_type==HAL_TCL_ENCAP_TYPE_NATIVE_WIFI);\n\tath11k_hal_tx_cmd_desc_setup(ab, hal_tcl_desc +')
    dp=once(dp,'if (unlikely(ts.buf_rel_source == HAL_WBM_REL_SRC_MODULE_FW)) {','''if (unlikely(ts.buf_rel_source == HAL_WBM_REL_SRC_MODULE_FW)) {
			struct htt_tx_wbm_completion *fw=(void *)((u8 *)tx_status+HTT_TX_WBM_COMP_STATUS_OFFSET);
			unsigned status=FIELD_GET(HTT_TX_WBM_COMP_INFO0_STATUS,fw->info0);
			if (lx_awdl_echo_private() && mac_id < ab->num_radios && ab->pdevs[mac_id].ar) {
				spin_lock(&tx_ring->tx_idr_lock);
				struct sk_buff *tracked=idr_find(&tx_ring->txbuf_idr,msdu_id);
				lx_awdl_echo_wbm(ab->pdevs[mac_id].ar->hw,tracked,ring_id,desc_id,
				    ts.buf_rel_source,status,0,0);
				spin_unlock(&tx_ring->tx_idr_lock);
			}''')
    dp=once(dp,'ath11k_dp_tx_complete_msdu(ar, msdu, &ts);','''lx_awdl_echo_wbm(ar->hw,msdu,ring_id,desc_id,ts.buf_rel_source,
		    ts.status,ts.try_cnt,ts.buf_rel_source==HAL_WBM_REL_SRC_MODULE_TQM &&
		    ts.status==HAL_WBM_TQM_REL_REASON_FRAME_ACKED);
		ath11k_dp_tx_complete_msdu(ar, msdu, &ts);''')
    write(vendor+'dp_tx.c',dp)
    bus=read(src+'lx_bus.c')
    bus=once(bus,'static void\ndriver_says(', 'extern int lx_awdl_echo_private(void);\nstatic int awdl_addr_format(const char *f) { for (; *f; f++) if (f[0]==37 && f[1]==112 && f[2]==77) return 1; return 0; }\nstatic void\ndriver_says(')
    msg='if (lx_awdl_echo_private() && awdl_addr_format(format)) { lx_tell("ath11k: address-bearing message omitted during controlled transfer\\n"); return; }\n\t'
    bus=bus.replace('vsnprintf(line, sizeof(line), format, list);',msg+'vsnprintf(line, sizeof(line), format, list);')
    write(src+'lx_bus.c',bus)
    subprocess.run(['docker','exec',BUS,'rm',*[work+'obj/'+f+'.o' for f in ('lx_lx_80211','lx_lx_bus','ath11k_mac','ath11k_peer','ath11k_wmi','ath11k_dp_tx')]],check=True)
    with (OUT/'data-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    for token,s in [(MAC_TOKEN,mac),(IP_TOKEN,ip)]:
        if linked.count(token)!=1: raise ValueError('compiled placeholder was not unique')
        linked=linked.replace(token,s)
    dest=OUT/'ath11k-cell-data-peer.o'
    temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-peer-build.json').write_text(json.dumps({'endpoint_source':endpoint_source,'reference':PRIOR,'sha256':sha(dest),'object':str(dest),
        'builder':sha(Path(__file__)),'driver':sha(HERE/'data.inc'),'container_tree':job},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
