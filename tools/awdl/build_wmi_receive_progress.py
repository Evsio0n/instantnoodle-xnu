"""Base peer visits on actual native upload progress, including RX-only bursts."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-receiver-Qgexk3'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='d386f20a54697614f212bde704139f8e0840b6ada90486a3a04315437f0db6bc'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-rx-progress-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'live_transport.inc')
s=once(s,'static unsigned live_once_rx_ip;', (HERE/'receive_window.inc').read_text()+'\nstatic unsigned live_once_rx_ip;')
s=once(s,'bool mdns_pending=', 'if(mdns_slot>=0 && live_mac_local(da))receive_note((unsigned)mdns_slot,ip,n,mdns_now);\n    bool mdns_pending=')
write(src+'live_transport.inc',s)
s=read(src+'auto_owner.inc')
s=once(s,'struct live_fifo *q=live.tx.count?', '''int rx=receive_choice(now);
    if(rx>=0 && auto_find_locked(air.table.peers[rx].data_mac,now)==rx) {
        uint64_t serial=air.table.peers[rx].serial;
        if(air.active && air.active_serial==serial) {
            /* Within a real upload, give the sender time to send its next
             * burst even when its preceding ACK emptied the transmit FIFO. */
            lx_os_unlock();return;
        }
        if(!live_count_flights_locked(0)) {
            air.wanted=(unsigned)rx;air.wanted_serial=serial;
        }
        lx_os_unlock();return;
    }
    struct live_fifo *q=live.tx.count?''')
write(src+'auto_owner.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-rx-progress-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-rx-progress.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
