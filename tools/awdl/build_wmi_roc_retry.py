"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-native-service-hD5zpa'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='6d5fb250ce859787c3e4f8b24bcdd66c2561c2cbfe549507a4d708586aedab61'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-roc-retry-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'auto_peers.inc')
s=once(s,'static unsigned auto_phy_chan(', (HERE/'roc_backoff.inc').read_text()+'\nstatic unsigned auto_phy_chan(')
s=once(s,'home && auto_channel(r,home,true)','home && !roc_recently_unavailable(home,now) && auto_channel(r,home,true)')
s=once(s,'if(!ch || !auto_channel(air.radio,ch,true))continue;', 'if(!ch || roc_recently_unavailable(ch,now) || !auto_channel(air.radio,ch,true))continue;')
write(src+'auto_peers.inc',s)
s=read(src+'auto_owner.inc')
s=once(s,'if(auto_channel(air.radio,air.channel,true))','if(!roc_recently_unavailable(air.channel,now) && auto_channel(air.radio,air.channel,true))')
s=once(s,'if(channel!=carrier && auto_channel(r,channel,true))','if(channel!=carrier && !roc_recently_unavailable(channel,lx_os_nanoseconds()/1000ull) && auto_channel(r,channel,true))')
write(src+'auto_owner.inc',s)
s=read(src+'roc.inc')
s=once(s,'    lx_tell("awdl-roc-time: begin_us=', '    roc_record_outcome(channel_of(target),ROC_GET(r->foreign)!=0,rc==-ETIMEDOUT && ROC_GET(r->done) && !ROC_GET(r->foreign),lx_os_nanoseconds()/1000ull);\n    lx_tell("awdl-roc-time: begin_us=')
write(src+'roc.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-roc-retry-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-roc-retry.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
