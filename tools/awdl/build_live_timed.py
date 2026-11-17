"""Bound the lifetime of the integrated radio and route FW loss by the originating VIF."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='e8a9a1898f76d860f08c8c7302159a0a34b06eaeeb1486cf7c8d8518996bd9e5'

def build():
    pin=json.loads((OUT/'live-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected first live integration')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-life-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    d=stage+'/wifi/lx/src/'
    p=d+'live_transport.inc'; code=read(p)
    code=once(code,'static unsigned live_infra_window;','''static unsigned live_infra_window,live_timeout,live_bmiss,live_loss;
static void live_event(struct lx_radio *radio,struct ieee80211_vif *vif,unsigned kind)
{
	if (!radio || !radio->persistent_awdl || !EG(live.running)) return;
	if (!vif || vif==radio->vif) {
		ES(live.stop,1);lx_os_wakeup(&bw.signal);return;
	}
	if (EG(radio->roc.visible)!=vif) return;
	unsigned prior=kind==1?EA(live_bmiss):EA(live_loss);
	if (!prior) lx_tell("awdl-live: secondary_link_event=%u primary_kept=1\\n",kind);
}
static bool live_deadline(void)
{
	const uint64_t now=lx_os_nanoseconds()/1000ull;
	if (EG(live.armed) && now>=live_first_us && now-live_first_us>=165000000ull) {
		ES(live_timeout,1);ES(live.stop,1);return true;
	}
	return false;
}''')
    code=once(code,'if (!EG(live.armed) || EG(live.stop)) return;', 'if (!EG(live.armed) || EG(live.stop) || live_deadline()) return;')
    code=once(code,'while (!EG(live.stop)) {','while (!EG(live.stop) && !live_deadline()) {')
    code=once(code,'ES(live.ready,0);(void)lx_os_awdl_set_ready','lx_tell("awdl-live: ending lifetime_limit_s=165 timer=%u secondary_bmiss=%u secondary_lost=%u\\n",EG(live_timeout),EG(live_bmiss),EG(live_loss));\n\tES(live.ready,0);(void)lx_os_awdl_set_ready')
    write(p,code)
    q=d+'lx_80211.c'; code=read(q)
    code=once(code,'static void live_stop(struct lx_radio *);','static void live_stop(struct lx_radio *);\nstatic void live_event(struct lx_radio *,struct ieee80211_vif *,unsigned);')
    for name,typ in [('beacon_loss',1),('connection_loss',2)]:
        old='linuxkpi_ieee80211_'+name+'(struct ieee80211_vif *vif)\n{'
        code=once(code,old,old+'\n\tstruct lx_radio *r=the_radio;\n\tif (r && r->persistent_awdl && vif && vif!=r->vif) {live_event(r,vif,'+str(typ)+');return;}')
    a=code.index('link_lost(struct lx_radio *radio, int error, const char *why)\n{')
    b=code.index('radio->joined = false;',a)
    code=code[:b]+'if (radio->persistent_awdl) live_event(radio,radio->vif,0);\n\t'+code[b:]
    # Firmware recovery drops station objects on its own path.
    a=code.index('restart_runs(struct work_struct *work)\n{')
    b=code.index('hw = &radio->hw;',a)
    code=code[:b]+'if (radio->persistent_awdl) live_stop(radio);\n\t'+code[b:]
    write(q,code)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'live-timed-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-live-timed.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'live-timed-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
