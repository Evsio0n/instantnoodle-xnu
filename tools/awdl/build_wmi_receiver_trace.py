"""Trace native inbound handshake replies across ifnet, queue and firmware."""
import subprocess
from build_roc import BUS, OUT, read, write, once, sha

base = '/tmp/awdl-handoff-commit-oDfTth'
obj = '/xnu-deps/build/ath11k/out/ath11k.o'
expected = 'c3d1c82143a54ee24d941bfc2b090468364ce3677977557af0d9fbea62eade7c'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0] == expected
stage = subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-receiver-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
path = stage+'/wifi/lx/src/live_transport.inc'
s = read(path)
s = once(s,'static int live_output(','''static unsigned live_wire_u32(const uint8_t *p)
{
    return ((unsigned)p[0]<<24) | ((unsigned)p[1]<<16) | ((unsigned)p[2]<<8) | p[3];
}

static int live_output(''')
s = once(s,'if(!group && !auto_route(frame,frame+38)) return EHOSTUNREACH;', '''bool syn=n>=74 && frame[20]==6 && (frame[67]&2);
    if(!group && !auto_route(frame,frame+38)) {
        if(syn)lx_tell("awdl-handshake: route_failed seq=%u ack=%u flags=%u\\n",
            live_wire_u32(frame+58),live_wire_u32(frame+62),frame[67]);
        return EHOSTUNREACH;
    }''')
s = once(s,'depth=q->count;\n\tlx_os_unlock();','''depth=q->count;
    lx_os_unlock();
    if(syn)lx_tell("awdl-handshake: enqueue ticket=%u seq=%u ack=%u flags=%u rc=%d\\n",
        tag,live_wire_u32(frame+58),live_wire_u32(frame+62),frame[67],error);''')
s = once(s,' && !(ip[53] & 16)', '')
s = once(s,'uint8_t expected[16];\n            lx_peer_make_eui64','''lx_tell("awdl-handshake: posting ticket=%u seq=%u ack=%u flags=%u queue_us=%llu\\n",
                f->tag,live_wire_u32(ip+44),live_wire_u32(ip+48),ip[53],now-f->queued_us);
            uint8_t expected[16];
            lx_peer_make_eui64''')
write(path,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-receiver-trace-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest = OUT/'ath11k-wmi-receiver-trace.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
