"""Bind the private provisioned radio carrier to XNU's real transport API."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,ROOT,OUT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='23f480f1523b61a62046b8138f14ccc56802c9830bf187b4cc2e9011c6c675f2'

def build():
    pin=json.loads((OUT/'data-nd-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR:raise ValueError('expected #428')
    base=pin['container_tree'];target=ROOT/'out/airdrop'
    ip=(target/'controlled-mac-ipv6.bin').read_bytes();peer=(target/'controlled-mac-derived-address.bin').read_bytes()
    if len(peer)!=6 or len(ip)!=16 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('private endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1:raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    mark=OUT/'live-stage.txt'
    if mark.exists(): stage=mark.read_text().strip()
    else:
        stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-live-XXXXXX'],text=True).strip()
        mark.write_text(stage+'\n')
    if not stage.startswith('/tmp/awdl-live-') or '/' in stage.removeprefix('/tmp/awdl-live-'):raise ValueError('stage')
    subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/';src=stage+'/wifi/lx/src/'
    if equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))!=Path(pin['object']).read_bytes():raise ValueError('baseline')
    for name in ['lx_awdl_transport.h','ifnet_transport.h','live_transport.inc','awdl_discovery.h','awdl_rx_action.h','peer_windows.h']:
        write(src+name,(HERE/name).read_text())
    lx=read(src+'lx_80211.c')
    lx=once(lx,'struct lx_awdl_roc_state roc;', 'bool persistent_awdl;\n\tstruct lx_awdl_roc_state roc;')
    lx=once(lx,'radio->roc.choice=lx_os_option("wifi_awdl_observe");','''radio->roc.choice=lx_os_option("wifi_awdl_observe");
	radio->persistent_awdl=radio->roc.choice==49;
	if (radio->persistent_awdl) radio->roc.choice=48;''')
    lx=once(lx,'void\nlinuxkpi_ieee80211_unregister_hw(', 'static void live_stop(struct lx_radio *);\nvoid\nlinuxkpi_ieee80211_unregister_hw(')
    for name in ['linuxkpi_ieee80211_unregister_hw(struct','station_leave(struct lx_radio *radio)\n{']:
        a=lx.index(name)
        if name.startswith('station'):at=lx.index('if (radio->joined) {',a)
        else:at=lx.index('lx_os_lock();',a)
        lx=lx[:at]+'if (radio->persistent_awdl) live_stop(radio);\n\t'+lx[at:]
    write(src+'lx_80211.c',lx)
    data=read(src+'data.inc')
    data=once(data,'static bool lx_awdl_listen_channel(','''static struct ieee80211_vif *live_window_vif(struct lx_radio *,struct ieee80211_vif *);
static void live_tick(struct lx_radio *,struct ieee80211_vif *,struct linuxkpi_ieee80211_channel *,unsigned);
static bool lx_awdl_listen_channel(''')
    data=once(data,'unsigned armed,finished,roc_ok,signal','unsigned control_id,armed,finished,roc_ok,signal')
    data=once(data,'r->vdev==echo.vdev && EG(r->ready)','r->vdev==bw.control_id && EG(r->ready)')
    data=once(data,'if (EG(bw.last_kind)!=2 ||','if (bw.control_id!=echo.vdev || EG(bw.last_kind)!=2 ||')
    data=once(data,'echo.vdev!=vdev) return;\n\tif (kind==LX_ROC_START)', 'bw.control_id!=vdev) return;\n\tif (kind==LX_ROC_START)')
    data=once(data,'bw.radio=radio; bw.vif=vif; bw.own_mhz=ch->center_freq;', 'bw.radio=radio; bw.vif=live_window_vif(radio,vif); bw.control_id=lx_awdl_roc_vdev(bw.vif); bw.own_mhz=ch->center_freq;')
    data=once(data,'unsigned basis=lx_awdl_home_basis(hw);','unsigned basis=lx_awdl_home_basis(hw);\n\t\tif (radio->persistent_awdl) live_tick(radio,vif,ch,basis);')
    data=once(data,'void lx_awdl_echo_wbm(', '#include "awdl_discovery.h"\n#include "live_transport.inc"\nvoid lx_awdl_echo_wbm(')
    data=once(data,'bool unicast=!memcmp(da,echo.mac,6);','if (EG(live.armed)) return live_rx_ipv6(da,ip,bytes);\n\tbool unicast=!memcmp(da,echo.mac,6);')
    data=once(data,'if (n<14 || memcmp(p+6,awdl_target_mac,6) || !lx_awdl_nd_l2(p)) return false;',
        'if (n<14 || memcmp(p+6,awdl_target_mac,6) || (!EG(live.armed) && !lx_awdl_nd_l2(p))) return false;')
    data=once(data,'!lx_awdl_nd_l2(p+4) || memcmp(p+10,awdl_target_mac,6) ||','(!EG(live.armed) && !lx_awdl_nd_l2(p+4)) || memcmp(p+10,awdl_target_mac,6) ||')
    data=once(data,'if (!port) {\n\t\tES(echo.nd_watch,1);','''if (!port && radio->persistent_awdl) {
		if (!live_service(radio,vif,peer,ch)) return false;
	} else if (!port) {
		ES(echo.nd_watch,1);''')
    marker='if ((!up && down) || (EG(echo.submitted) && !EG(echo.released))) {'
    data=once(data,marker,'if (radio->persistent_awdl) live_withdraw();\n\t'+marker)
    write(src+'data.inc',data)
    roc=read(src+'roc.inc')
    roc=once(roc,'r->vdev==echo.vdev) { ES(bw.ready,1);','r->vdev==bw.control_id) { ES(bw.ready,1);')
    a=roc.index('lx_awdl_psf_prepare(struct');at=roc.index('if (__atomic_load_n(&r->tx_frame',a)
    roc=roc[:at]+'if (EG(live.mgmt_frame)==skb) return live_mgmt_prepare(hw,skb,vdev,desc,frequency);\n\t'+roc[at:]
    a=roc.index('lx_awdl_psf_submit_result(struct');at=roc.index('if (!ROC_GET(r->tx_bound)',a)
    roc=roc[:at]+'''if (EG(live.mgmt_pending) && EG(live.mgmt_bound) && EG(live.mgmt_desc)==desc) {
		ES(live.mgmt_rc,rc);EA(live.mgmt_submitted);if(rc)EA(live_wmi_errors);return;
	}
	'''+roc[at:]
    a=roc.index('lx_awdl_psf_fw(struct');at=roc.index('if (__atomic_load_n(&r->tx_frame',a)
    roc=roc[:at]+'''if (EG(live.mgmt_frame)==skb && EG(live.mgmt_bound) && EG(live.mgmt_desc)==desc) {
		ES(live.mgmt_status,status);ES(live.mgmt_fw,1);
		if (!status) { if (EG(live.mgmt_phy)==3) EA(live_wmi_mif); else EA(live_wmi_psf); }
		else EA(live_wmi_errors);
		return;
	}
	'''+roc[at:]
    a=roc.index('lx_awdl_psf_release(struct');at=roc.index('struct sk_buff *expected=skb;',a)
    roc=roc[:at]+'''if (EG(live.mgmt_frame)==skb) {
		ES(live.mgmt_frame,NULL);
		if (dropped || !EG(live.mgmt_fw)) EA(live_wmi_drop);
		else if (!EG(live.mgmt_status)) ES(live.next_mif,EG(live.mgmt_phy)!=3);
		ES(live.mgmt_pending,0);return;
	}
	'''+roc[at:]
    roc=once(roc,'static void\nlx_awdl_listen_once(struct lx_radio *radio)','static void\nlx_awdl_listen_owner(struct lx_radio *radio)')
    roc+='''\nstatic void live_thread(void *context)
{
	struct lx_radio *r=context;
	lx_awdl_listen_owner(r);
	live_withdraw();
	ES(live.ended,1);lx_os_wakeup(&live.ended);
}
static void lx_awdl_listen_once(struct lx_radio *r)
{
	if (!r->persistent_awdl) {lx_awdl_listen_owner(r);return;}
	if (EG(live.running)) return;
	live.radio=r;ES(live.running,1);
	int e=lx_os_thread("wifi-awdl-owner",live_thread,r);
	if(e){ES(live.running,0);lx_tell("awdl-live: owner spawn rc=%d\\n",e);}
}
static void live_stop(struct lx_radio *r)
{
	if (!EG(live.running) || live.radio!=r) return;
	ES(live.stop,1);lx_os_wakeup(&bw.signal);
	lx_os_lock();
	while (!EG(live.ended)) lx_os_sleep(&live.ended,50000000ull);
	lx_os_unlock();
}'''
    write(src+'roc.inc',roc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'live-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    binary=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-live.o';tmp=dest.with_suffix('.new')
    with os.fdopen(os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:f.write(binary)
    os.replace(tmp,dest)
    (OUT/'live-build.json').write_text(json.dumps({'sha256':sha(dest),'object':str(dest),'container_tree':stage,
        'parent':PRIOR,'owner':sha(HERE/'live_transport.inc'),'builder':sha(Path(__file__)),'mode':49},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__':build()
