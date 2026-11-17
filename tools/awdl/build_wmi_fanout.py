"""Deliver logical IPv6 multicast over acknowledged per-peer AWDL frames.

The native producer and packet bytes remain multicast. The automatic owner
already queues one copy per fresh observed link recipient. Use that real
recipient as the physical receiver and its normal unicast availability
window, avoiding this firmware's discarded group-data command contexts.
"""
import json,subprocess,argparse
from build_roc import BUS,OUT,read,write,once,sha
BASE='/tmp/awdl-wmi-data-CVAE93'
BASE_SHA='f4aa17e8c773c7469c38f2066bc54b52cf6e28531ef2bb128260369cf4622938'
def build(long=False):
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('TCP-verified WMI data base changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-wmi-fanout-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/'
    s=read(src+'live_transport.inc')
    s=once(s,'if (!q->count || (lane&&!follow_multicast_due_locked(now))) continue;',
        '''/* Physical unicast copies use the common guarded peer window;
         * the outer gate still requires a real channel grant and live peer. */
        if (!q->count) continue;''')
    s=once(s,'memset(frame,0,24);frame[0]=8;memcpy(frame+4,live.scratch.bytes,6);',
        '''memset(frame,0,24);frame[0]=8;memcpy(frame+4,live.scratch.bytes,6);
        if(f->group)memcpy(frame+4,live.scratch.link_target,6);''')
    s=once(s,'f->wire_group=!!(frame[4]&1);',
        '''f->wire_group=!!(frame[4]&1);
        if(f->group)lx_tell("awdl-wmi-data: multicast_unicast_copy ticket=%u recipient_current=%u ipv6_multicast=%u\\n",f->tag,!memcmp(frame+4,active_peer_mac,6),frame[64]==0xff);''')
    if long:s=once(s,'now-live_first_us>=420000000ull','now-live_first_us>=900000000ull')
    write(src+'live_transport.inc',s)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/('wmi-fanout-long-build.log' if long else 'wmi-fanout-build.log')).open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/('ath11k-wmi-fanout-long.o' if long else 'ath11k-wmi-fanout.o');subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,acknowledged_multicast_fanout=True,offchan_data_wmi=True,additional_peer=False,fixed_endpoint=False,explicit_country=True,lifetime_seconds=900 if long else 420)
    (OUT/('wmi-fanout-long-build.json' if long else 'wmi-fanout-build.json')).write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--long',action='store_true');build(p.parse_args().long)
