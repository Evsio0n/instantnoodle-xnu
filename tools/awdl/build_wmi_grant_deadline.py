"""Bound obsolete owned ROC waits; let firmware completion end an AWDL visit."""
import json
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-raw-scan-cX7h9v'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='4a4f103c58bb48f3017b0787e375998bd1a7d34c2593e72429eb9daffeb8fae8'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-grant-deadline-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
write(src+'roc_deadline.h',(HERE/'roc_deadline.h').read_text())
p=src+'roc_state.h';s=read(p)
s=once(s,'unsigned choice, attempted, active;', 'unsigned choice, attempted, active;\n    uint64_t latest_grant_us;')
write(p,s)
p=src+'roc.inc';s=read(p)
s=once(s,'static uint64_t slot_opened,slot_closed;', '''#include "roc_deadline.h"
static uint64_t slot_opened,slot_closed;
unsigned long long lx_awdl_latest_grant_us(struct ieee80211_hw *hw,unsigned token)
{
    struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
    return ROC_GET(r->active) && token && token==ROC_GET(r->request_seq)?
        ROC_GET(r->latest_grant_us):0;
}''')
s=once(s,'uint64_t receipt_begin=lx_os_nanoseconds()/1000ull;', '''uint64_t receipt_begin=lx_os_nanoseconds()/1000ull;
    ROC_SET(r->latest_grant_us,awdl_latest_grant(receipt_begin,plan.serial?plan.end:0));''')
s=once(s,'while (ROC_GET(r->done) && !ROC_GET(r->home_beacon)', 'while (!radio->persistent_awdl && ROC_GET(r->done) && !ROC_GET(r->home_beacon)')
write(p,s)
p=stage+'/xnu-deps/build/ath11k/src/ath11k/mac.c';s=read(p)
s=once(s,'static struct ath11k *awdl_claim_ar;', 'extern unsigned long long lx_os_nanoseconds(void);\nextern unsigned long long lx_awdl_latest_grant_us(struct ieee80211_hw *,unsigned);\nstatic struct ath11k *awdl_claim_ar;')
s=once(s,'unsigned long deadline = jiffies + 3 * HZ;', '''unsigned long deadline = jiffies + 3 * HZ;
        unsigned long long latest=lx_awdl_latest_grant_us(hw,awdl_claim_seq);''')
s=once(s,'            if (time_after_eq(jiffies, deadline)) break;', '''            if (latest && lx_os_nanoseconds()/1000ull>=latest) {
                lx_tell("awdl-roc-obsolete: token=%u latest=%llu now=%llu\\n",
                    awdl_claim_seq,latest,lx_os_nanoseconds()/1000ull);
                ret=0;break;
            }
            if (time_after_eq(jiffies, deadline)) break;''')
write(p,s)
for name in ('ath11k_mac.o','lx_lx_80211.o','ath11k_wmi.o'):
 subprocess.run(['docker','exec',BUS,'rm','-f',stage+'/xnu-deps/build/ath11k/obj/'+name],check=True)
with (OUT/'wmi-grant-deadline-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-grant-deadline.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-grant-deadline-build.json').write_text(json.dumps(receipt,indent=2))
print(json.dumps(receipt,indent=2))
