"""Bounded original-query retransmission during native AirDrop TCP activity."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-mdns-unicast-b6wyfA'
BASE_SHA='1f242bfebf0c15a0bc1f1d04eb90eb6dba101edc921f96a3553c0e50f8781a30'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('carrier control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-mdns-refresh-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'live_transport.inc')
    s=once(s,'#define LIVE_MTU 1500u', '''#define LIVE_MTU 1500u
struct mdns_refresh_cache {uint64_t serial,lease,wire_us,next_us;size_t n;uint8_t bytes[14+LIVE_MTU];};
static struct mdns_refresh_cache mdns_refresh[LX_AWDL_PEER_SLOTS];
static uint64_t mdns_transfer_serial[LX_AWDL_PEER_SLOTS],mdns_transfer_until[LX_AWDL_PEER_SLOTS];
static unsigned mdns_refresh_sent;''')
    anchor='''        if(mdns_peer.serial==air.active_serial && hk_mdns_query_unicast(live.scratch.bytes,live.scratch.n,live.scratch.link_target,mdns_ip)) {'''
    code='''        bool mdns_airdrop=false;
        if(hk_mdns_shape(live.scratch.bytes,live.scratch.n,false)) {
            static const uint8_t label[]={8,'_','a','i','r','d','r','o','p'};
            for(size_t at=62;at+sizeof(label)<=live.scratch.n;at++)
                if(!memcmp(live.scratch.bytes+at,label,sizeof(label))) {mdns_airdrop=true;break;}
        }
        if(mdns_airdrop && mdns_peer.serial==air.active_serial) {
            lx_os_lock();struct mdns_refresh_cache *cache=&mdns_refresh[air.active_slot];
            cache->serial=mdns_peer.serial;cache->lease=live.lease;cache->wire_us=now;
            cache->next_us=now+3000000ull;cache->n=live.scratch.n;memcpy(cache->bytes,live.scratch.bytes,live.scratch.n);
            lx_os_unlock();
        }
'''+anchor
    s=once(s,anchor,code)
    marker='\tconst uint8_t *ip = frame + 40;'
    s=once(s,marker,marker+'''
        if(bytes>=60 && ip[6]==6 && (((unsigned)ip[40]*256+ip[41])==8770 || ((unsigned)ip[42]*256+ip[43])==8770)) {
            lx_os_lock();mdns_transfer_serial[air.active_slot]=air.active_serial;mdns_transfer_until[air.active_slot]=now+15000000ull;lx_os_unlock();
        }
''')
    s=once(s,'static void live_tick(', (HERE/'mdns_refresh.inc').read_text()+'\nstatic void live_tick(')
    s=once(s,'live_retire();live_deliver();live_presence();','live_retire();live_deliver();live_presence();mdns_refresh_tick();')
    write(src+'live_transport.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-mdns-refresh-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-mdns-refresh.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,native_query_retransmit=True,answer_payload_unchanged=True,active_tcp8770_only=True,wire_query_retry_epoch_seconds=30,retry_seconds=3,lifetime_seconds=900)
    (OUT/'wmi-mdns-refresh-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
