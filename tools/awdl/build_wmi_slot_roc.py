"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-roc-retry-GmKazJ'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='44fc307bce90c5617fb7d5bc1b149db65bb638addbccba1a47f3cd38f163b81f'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-slot-roc-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'roc.inc')
s=once(s,'unsigned lx_awdl_request_token(', 'static uint64_t slot_opened,slot_closed;\n'+(HERE/'slot_roc.inc').read_text()+'\nunsigned lx_awdl_request_token(')
s=once(s,'ROC_ADD(r->foreign);', 'if(!ROC_GET(slot_opened))ROC_SET(slot_opened,ROC_GET(r->foreign_us));\n            ROC_ADD(r->foreign);')
s=once(s,'switch (event) {','''if((event==LX_ROC_BSS || event==LX_ROC_DONE || event==LX_ROC_DEQUEUED) &&
        ROC_GET(slot_opened) && !ROC_GET(slot_closed))ROC_SET(slot_closed,lx_os_nanoseconds()/1000ull);
    switch (event) {''')
s=once(s,'uint64_t receipt_begin=lx_os_nanoseconds()/1000ull;','''uint64_t now=lx_os_nanoseconds()/1000ull;
    struct slot_target plan=slot_target_for(c,now);
    while(plan.issue>now && !EG(live.stop)){
        unsigned ms=(unsigned)((plan.issue-now+999)/1000);
        lx_pause_ms(ms>12?12:ms);
        now=lx_os_nanoseconds()/1000ull;
        if(now>=plan.issue)break;
        plan=slot_target_for(c,now);
    }
    if(EG(live.stop))return false;
    ROC_SET(slot_opened,0);ROC_SET(slot_closed,0);
    uint64_t receipt_begin=lx_os_nanoseconds()/1000ull;''')
s=once(s,'roc_record_outcome(channel_of(target)','''uint64_t opened=ROC_GET(slot_opened),closed=ROC_GET(slot_closed);
    if(c<178 && opened>=receipt_begin && opened-receipt_begin<=3000000)
        slot_sample(c,(unsigned)(opened-receipt_begin));
    if(plan.serial){
        uint64_t lo=plan.start>opened?plan.start:opened,hi=plan.end<closed?plan.end:closed;
        uint64_t overlap=opened && closed>opened && hi>lo?hi-lo:0;
        lx_tell("awdl-slot: peer=%llu ch=%u start=%llu end=%llu lead=%u issue=%llu open=%llu close=%llu overlap=%llu\\n",
            plan.serial,c,plan.start,plan.end,plan.lead,receipt_begin,opened,closed,overlap);
    }
    roc_record_outcome(channel_of(target)''')
write(src+'roc.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-slot-roc-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-slot-roc.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
