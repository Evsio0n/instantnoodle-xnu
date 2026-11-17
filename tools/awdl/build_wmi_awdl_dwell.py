"""Use the AWDL owner dwell for the firmware passive scan request."""
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
base='/tmp/awdl-destination-tgtmkE'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='892fa960be8484a710ad9fb877c0748c65aab0955f101b8fa6786d1272a45b4d'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-dwell-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
driver=stage+'/xnu-deps/build/ath11k/src/ath11k/mac.c'
s=read(driver)
s=once(s,'arg->burst_duration = duration;','''arg->burst_duration = duration;
    if (awdl_claim_ar == ar && awdl_claim_seq) {
        arg->dwell_time_active = duration;
        arg->dwell_time_passive = duration;
        lx_tell("awdl-dwell: token=%u freq=%u passive_ms=%u burst_ms=%u timeout_ms=%u\\n",
            awdl_claim_seq,chan->center_freq,arg->dwell_time_passive,
            arg->burst_duration,arg->max_scan_time);
    }''')
write(driver,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/ath11k_mac.o'],check=True)
with (OUT/'wmi-dwell-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-dwell.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
