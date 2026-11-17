"""Use the AWDL owner dwell for the firmware passive scan request."""
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
base='/tmp/awdl-dwell-JmBMTY'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='e9f2ecd72dad1d2de83f134ce492e34f5fd524d030a601c308c0b1f5cfd26dcd'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-raw-scan-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
driver=stage+'/xnu-deps/build/ath11k/src/ath11k/wmi.c'
s=read(driver)
s=once(s,'static void ath11k_scan_event(struct ath11k_base *ab, struct sk_buff *skb)\n{','''extern unsigned long long lx_os_nanoseconds(void);
static unsigned scan_trace_event;
static void ath11k_scan_event(struct ath11k_base *ab, struct sk_buff *skb)
{''')
s=once(s,'\trcu_read_lock();\n\n\t/* In case the scan was cancelled','''    unsigned serial=__atomic_add_fetch(&scan_trace_event,1,__ATOMIC_RELAXED);
    lx_tell("awdl-wmi-raw: n=%u us=%llu type=%u req=%u scan=%u vdev=%u freq=%u reason=%u\\n",
        serial,lx_os_nanoseconds()/1000ull,scan_ev.event_type,scan_ev.scan_req_id,
        scan_ev.scan_id,scan_ev.vdev_id,scan_ev.channel_freq,scan_ev.reason);
    rcu_read_lock();

    /* In case the scan was cancelled''')
write(driver,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/ath11k_wmi.o'],check=True)
with (OUT/'wmi-raw-scan-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-raw-scan.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
