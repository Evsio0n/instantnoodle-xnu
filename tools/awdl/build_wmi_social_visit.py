"""Visit each enabled advertised social channel when the receiver is idle."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-rx-progress-QR6or7'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='c326535b401246dcce823e9314b6e5f80709e5b38e9061c80099ca22feb58dc0'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-social-visit-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'auto_owner.inc')
s=once(s,'static bool auto_window(', (HERE/'social_visit.inc').read_text()+'\nstatic bool auto_window(')
s=once(s,'channel==6 && auto_channel(r,6,true)', 'auto_channel(r,channel,true)')
s=once(s,'if(!air.prepeer && ch->center_freq!=2437 &&','if(!air.prepeer &&')
s=once(s,'if(!auto_window(r,v,6)) {ES(live.stop,1);break;}','''uint64_t visit_now=lx_os_nanoseconds()/1000ull;
            lx_os_lock();
            bool receiving=receive_choice(visit_now)>=0;
            lx_os_unlock();
            if(!receiving) {
                unsigned social=social_visit(r,channel_of(ch->center_freq));
                if(social && !auto_window(r,v,social)) {ES(live.stop,1);break;}
                if(social)lx_tell("awdl-social: visit=%u carrier=%u\\n",social,channel_of(ch->center_freq));
            }''')
write(src+'auto_owner.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-social-visit-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-social-visit.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
