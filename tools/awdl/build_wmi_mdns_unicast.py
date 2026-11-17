"""Native mDNS multicast semantics over observed-peer unicast transport."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-phase-audit-nGhz2R'
BASE_SHA='642eb9615a87654035f36031de49f56ee6e4177ea26730e56e8a8133f5c33fb5'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('phase control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-mdns-unicast-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';write(src+'mdns_unicast.h',(HERE/'mdns_unicast.h').read_text())
    s=read(src+'live_transport.inc')
    s=once(s,'#include "native_flight.h"','#include "native_flight.h"\n#include "mdns_unicast.h"\nstatic uint64_t mdns_query_serial[LX_AWDL_PEER_SLOTS],mdns_query_until[LX_AWDL_PEER_SLOTS];\nstatic unsigned mdns_query_adapted,mdns_reply_restored;')
    s=once(s,'\tsize_t bytes=live.scratch.n-14;', '''    if(f->group) {
        lx_os_lock();struct lx_awdl_peer mdns_peer=air.table.peers[air.active_slot];lx_os_unlock();
        const uint8_t *mdns_ip=mdns_peer.has_observed_ipv6?mdns_peer.observed_ipv6:mdns_peer.eui64;
        if(mdns_peer.serial==air.active_serial && hk_mdns_query_unicast(live.scratch.bytes,live.scratch.n,live.scratch.link_target,mdns_ip)) {
            lx_os_lock();mdns_query_serial[air.active_slot]=mdns_peer.serial;mdns_query_until[air.active_slot]=now+15000000ull;lx_os_unlock();
            if(++mdns_query_adapted<=32)lx_tell("awdl-mdns: native_query_unicast ticket=%u peer=%llu\\n",f->tag,mdns_peer.serial);
        }
    }
\tsize_t bytes=live.scratch.n-14;''')
    anchor='pkt->bytes[12]=0x86;pkt->bytes[13]=0xdd;memcpy(pkt->bytes+14,ip,n);'
    s=once(s,anchor,anchor+'''
    uint64_t mdns_now=lx_os_nanoseconds()/1000ull;
    int mdns_slot=auto_find_locked(source,mdns_now);
    bool mdns_pending=mdns_slot>=0 && mdns_query_serial[mdns_slot]==air.table.peers[mdns_slot].serial && mdns_now<mdns_query_until[mdns_slot];
    if(hk_mdns_reply_multicast(pkt->bytes,pkt->n,echo.ip,mdns_pending) && ++mdns_reply_restored<=32)
        lx_tell("awdl-mdns: real_peer_reply_logical_multicast peer=%llu\\n",air.table.peers[mdns_slot].serial);
''')
    write(src+'live_transport.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-mdns-unicast-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-mdns-unicast.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,native_mdns_unicast_transport=True,dns_payload_unchanged=True,pending_peer_only_restore=True,lifetime_seconds=900)
    (OUT/'wmi-mdns-unicast-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
