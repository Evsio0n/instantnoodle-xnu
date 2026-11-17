"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-upload-owner-pjisoz'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='a13b06dc2f3345f517ca7bd1f12a182414bcdf081ac609dd815365bf9a40a65e'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-registration-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'live_transport.inc')
write(src+'service_registration.h',(HERE/'service_registration.h').read_text())
s=once(s,'static uint64_t details_until;', '#include "service_registration.h"')
start=s.index('if(!error && group && n>=74')
end=s.index('    depth=q->count;',start)
s=s[:start]+'''if(live_current(gen,lease) && live.accept && !live.stop && n>=74 && frame[20]==17 && frame[21]==255 &&
       ptr_u16(frame+54)==5353 && ptr_u16(frame+58)==n-54 && !memcmp(frame+22,echo.ip,16)){
        service_update(frame+62,n-62,&native_ptr,&native_details,now,&ptr_until);
        ptr_lease=lease;
    }
'''+s[end:]
s=once(s,'if(extra && now<details_until){','if(extra && native_details.srv_len && native_details.txt_len){')
s=once(s,'ptr_lease=0;details_until=0;', 'ptr_lease=0;memset(&native_ptr,0,sizeof(native_ptr));memset(&native_details,0,sizeof(native_details));')
s=once(s,'if(extra && logged++<12)', 'if(extra && (logged++<12 || !(logged%128)))')
write(src+'live_transport.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-registration-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-registration.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
