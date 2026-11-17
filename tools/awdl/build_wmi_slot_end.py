"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-slot-roc-ZsaYIo'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='f881f53c7138e87c5d2794bf8f8ba43a9be020a03390c2c88ec96faea226896f'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-slot-end-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
driver=stage+'/xnu-deps/build/ath11k/src/ath11k/mac.c'
s=read(driver)
s=once(s,'ret = wait_for_completion_timeout(&ar->scan.on_channel, 3 * HZ);','''if (awdl_claim_ar == ar && awdl_claim_seq) {
        unsigned long deadline = jiffies + 3 * HZ;
        for (;;) {
            ret = wait_for_completion_timeout(&ar->scan.on_channel, msecs_to_jiffies(20));
            if (ret) break;
            spin_lock_bh(&ar->data_lock);
            bool ended = ar->scan.state == ATH11K_SCAN_IDLE;
            spin_unlock_bh(&ar->data_lock);
            if (ended) {
                /* Firmware already finished this owned ROC without a grant.
                 * Do not wait the full timeout or stop an idle scan. */
                ret = -ETIMEDOUT;
                goto free_chan_list;
            }
            if (time_after_eq(jiffies, deadline)) break;
        }
    } else {
        ret = wait_for_completion_timeout(&ar->scan.on_channel, 3 * HZ);
    }''')
write(driver,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/ath11k_mac.o'],check=True)
with (OUT/'wmi-slot-end-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-slot-end.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
