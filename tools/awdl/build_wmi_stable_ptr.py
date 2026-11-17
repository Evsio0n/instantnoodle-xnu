"""Carry the locally registered native service PTR in discovery action frames."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-sync-election-pO26ds'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='53da35226e23c37135be68bbb187930fda59b3b3f34b8f83e26bb1e36b307950'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-service-ptr-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
write(src+'service_ptr.h',(HERE/'service_ptr.h').read_text())
s=read(src+'live_transport.inc')
s=once(s,'static int live_output(','''#include "service_ptr.h"
static struct awdl_local_ptr native_ptr;
static uint64_t ptr_lease,ptr_until;
static int live_output(''')
s=once(s,'depth=q->count;','''if(!error && group && n>=74 && frame[20]==17 && frame[21]==255 &&
       ptr_u16(frame+54)==5353 && ptr_u16(frame+56)==5353 && ptr_u16(frame+58)==n-54 &&
       !memcmp(frame+22,echo.ip,16) && !memcmp(frame+38,hk_mdns_ip,16)){
        struct awdl_local_ptr update={0};
        if(awdl_extract_ptr(frame+62,n-62,&update)){
            native_ptr=update;ptr_lease=lease;ptr_until=now+1000000ull*update.ttl;
            lx_tell("awdl-service: local_ptr length=%u ttl=%u\\n",update.length,update.ttl);
        }
    }
    depth=q->count;''')
s=once(s,'uint8_t data[LX_AWDL_DISCOVERY_MAX];','uint8_t data[LX_AWDL_DISCOVERY_MAX+78];')
s=once(s,'if (!n) return;\n\tbool directed=false;','''if (!n) return;
    lx_os_lock();
    unsigned extra=live.accept && ptr_lease==live.lease && now<ptr_until?
        awdl_append_ptr(data+n,sizeof(data)-n,&native_ptr):0;
    lx_os_unlock();
    n+=extra;
    static unsigned logged;
    if(extra && logged++<12)lx_tell("awdl-service: action subtype=%u ptr_bytes=%u channel=%u\\n",subtype,extra,channel_of(ch->center_freq));
    bool directed=false;''')
s=once(s,'live.lease=0;live.configured_notice=0;', 'live.lease=0;live.configured_notice=0;ptr_until=0;ptr_lease=0;')
write(src+'live_transport.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-stable-ptr-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-stable-ptr.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
