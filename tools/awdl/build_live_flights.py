"""Track native XNU packets through separate bounded group and unicast flight lanes."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='5f1420ff0a84c675e0b45a19a05c071e4fcc155a1ad07d98af3f7bf1b3f09558'

def build():
    pin=json.loads((OUT/'live-scoped-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #432')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-flights-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    d=stage+'/wifi/lx/src/'
    write(d+'native_flight.h',(HERE/'native_flight.h').read_text())
    code=read(d+'live_transport.inc')
    code=once(code,'#include "lx_awdl_transport.h"', '#include "lx_awdl_transport.h"\n#include "native_flight.h"')
    code=once(code,'struct live_frame { uint64_t lease;', 'struct live_frame { unsigned tag,proto; uint64_t queued_us,lease;')
    code=once(code,'uint64_t generation,lease,flight_lease;', 'uint64_t generation,lease;unsigned next_tag;')
    code=once(code,'struct live_fifo tx,rx;', 'struct live_fifo tx,groups,rx;')
    a=code.index('static int live_output(');b=code.index('static int live_channels(',a)
    code=code[:a]+'''static int live_output(void *ctx,uint64_t gen,uint64_t lease,const uint8_t *frame,size_t n)
{
	if (ctx!=&live || !frame || n<54 || n>14+LIVE_MTU || frame[12]!=0x86 || frame[13]!=0xdd ||
	    !lx_awdl_valid_ipv6(frame+14,n-14) || !live_mac_local(frame+6)) return EINVAL;
	bool group=live_group(frame,frame+14);
	if (!group && (!live_mac_peer(frame)||memcmp(frame+38,awdl_target_ip,16))) return EHOSTUNREACH;
	unsigned cls=live_proto_of(frame+14,n-14),tag=0,depth=0;
	lx_os_lock();
	struct live_fifo *q=group?&live.groups:&live.tx;
	int e=0;
	if (!live_current(gen,lease) || !live.accept || live.stop) e=ENETDOWN;
	else if (!live_put(q,lease,frame,n)) { live.queue_unavailable++;e=ENOBUFS; }
	else {
		struct live_frame *f=&q->slot[(q->first+q->count-1)%LIVE_DEPTH];
		f->queued_us=lx_os_nanoseconds()/1000ull; f->tag=tag=++live.next_tag;f->proto=cls;depth=q->count;
	}
	lx_os_unlock();
	lx_tell("awdl-ntx: enqueue ticket=%u class=%s group=%u lane_depth=%u rc=%d\\n",tag,live_proto_name(cls),group,depth,e);
	return e;
}
'''+code[b:]
    code=once(code,'static int live_quiesce(', 'static void live_retire(void);\nstatic unsigned live_count_flights_locked(uint64_t lease);\nstatic int live_quiesce(')
    a=code.index('static int live_quiesce(');b=code.index('static int live_close(',a)
    code=code[:a]+'''static int live_quiesce(void *ctx,uint64_t gen,uint64_t lease)
{
	if (ctx!=&live) return EINVAL;
	struct { unsigned id,cls,group;uint64_t born; } drop[LIVE_DEPTH*2];
	unsigned count=0;
	live_retire();lx_os_lock();
	if (!live.lease) { lx_os_unlock();return 0; }
	if (!live_current(gen,lease)) { lx_os_unlock();return ENOENT; }
	live.accept=0;live.closing=1;
	for(unsigned group=0;group<2;group++) {
		struct live_fifo *q=group?&live.groups:&live.tx;
		while(q->count) {
			struct live_frame *f=&q->slot[q->first];
			drop[count].id=f->tag;drop[count].cls=f->proto;drop[count].born=f->queued_us;
			drop[count++].group=group;live_pop(q);
		}
	}
	live.dropped+=count+live.rx.count;live.rx.count=0;
	int e=(live.calls||live_count_flights_locked(lease))?EBUSY:0;
	lx_os_unlock();
	uint64_t now=lx_os_nanoseconds()/1000ull;
	for(unsigned i=0;i<count;i++)lx_tell("awdl-ntx: withdrawn ticket=%u class=%s group=%u age_us=%llu submitted=0\\n",
	    drop[i].id,live_proto_name(drop[i].cls),drop[i].group,now-drop[i].born);
	return e;
}
'''+code[b:]
    code=once(code,'static unsigned live_flight_group;','')
    a=code.index('/* Called only by the long-lived radio owner');b=code.index('static int live_mgmt_prepare(',a)
    code=code[:a]+(HERE/'live_flights.inc').read_text().replace('static unsigned live_next_lane,live_next_tag;','static unsigned live_next_lane;')+'\n'+code[b:]
    code=code.replace('EG(live.flight_lease)','live_count_flights(0)')
    write(d+'live_transport.inc',code)
    code=read(src)
    a=code.index('void lx_awdl_echo_desc('); pos=code.index('\n{',a)
    code=code[:a]+'static void live_bound(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned,unsigned,int);\n'+code[a:pos+2]+'\n\tlive_bound(hw,skb,vdev,ring,desc,native);'+code[pos+2:]
    a=code.index('void lx_awdl_echo_wbm(');pos=code.index('\n{',a)
    code=code[:pos+2]+'\n\tlive_wbm(hw,skb,ring,desc,src,reason,attempts,acked);'+code[pos+2:]
    a=code.index('void lx_awdl_echo_release(');pos=code.index('\n{',a)
    code=code[:pos+2]+'\n\tlive_freed(hw,skb,local_drop);'+code[pos+2:]
    code=once(code,'if (!down && EG(echo.timedout)) {','if (!down && (EG(echo.timedout) || live_count_flights(0))) {')
    marker='while (!EG(echo.released) && lx_os_nanoseconds()<until) { lx_awdl_data_poll(hw); lx_pause_ms(10); }'
    code=once(code,marker,'''while (((EG(echo.submitted)&&!EG(echo.released)) || live_count_flights(0)) && lx_os_nanoseconds()<until) {
				live_retire(); lx_awdl_data_poll(hw); lx_pause_ms(10);
			}''')
    code=once(code,'if ((!up && down) || (EG(echo.submitted) && !EG(echo.released))) {',
        'if ((!up && down) || live_count_flights(0) || (EG(echo.submitted) && !EG(echo.released))) {')
    write(src,code)
    dp=work+'src/ath11k/dp_tx.c';code=read(dp)
    a=code.index('static void ath11k_dp_tx_free_txbuf(');b=code.index('\n}',a)
    part=once(code[a:b],'dev_kfree_skb_any(msdu);','lx_awdl_echo_release(ab->pdevs[mac_id].ar->hw,msdu,1);\n\tdev_kfree_skb_any(msdu);')
    write(dp,code[:a]+part+code[b:])
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_dp_tx.o'],check=True)
    with (OUT/'live-flights-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-live-flights.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'live-flights-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
