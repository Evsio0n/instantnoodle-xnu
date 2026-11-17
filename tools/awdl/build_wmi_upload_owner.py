"""Keep a bulk incoming AirDrop connection scheduled through HTTP completion."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-service-ptr-T2TIT4'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='e86b7d897b814940ca985d93b4815a13a3ae1a5a4b6aa0761088b31cdd23156f'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-upload-owner-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'live_transport.inc')
s=once(s,(HERE/'receive_window.inc').read_text(),(HERE/'receive_upload.inc').read_text())
write(src+'live_transport.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-upload-owner-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-upload-owner.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
