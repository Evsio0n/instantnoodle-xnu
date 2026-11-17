"""Use the AWDL owner dwell for the firmware passive scan request."""
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
base='/tmp/awdl-raw-scan-cX7h9v'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='4a4f103c58bb48f3017b0787e375998bd1a7d34c2593e72429eb9daffeb8fae8'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-priority-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
driver=stage+'/xnu-deps/build/ath11k/src/ath11k/mac.c'
s=read(driver)
s=once(s,'        arg->dwell_time_active = duration;', '        arg->scan_priority = WMI_SCAN_PRIORITY_HIGH;\n        arg->dwell_time_active = duration;')
write(driver,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/ath11k_mac.o'],check=True)
with (OUT/'wmi-roc-priority-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-roc-priority.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
