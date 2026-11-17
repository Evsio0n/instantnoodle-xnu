"""Follow a live canonical peer's changed channel without waiting for expiry."""
import json, subprocess
from build_roc import BUS, HERE, OUT, read, write, once, sha
BASE='/tmp/awdl-wmi-roc-home-nvHPos'
BASE_SHA='f64e14d663ef1b64b3c397a8d2741ec861afbb4aac3a8a473644012a15a45db5'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:
        raise ValueError('real-ROC control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-wmi-retune-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    path=stage+'/wifi/lx/src/auto_owner.inc';owner=read(path)
    owner=once(owner,'static void auto_mark_ready(', (HERE/'carrier_retune.inc').read_text()+'\nstatic void auto_mark_ready(')
    anchor='\t\tif(air.prepeer) {\n\t\t\tlx_os_lock();int found='
    owner=once(owner,anchor,'\t\tif(auto_follow_channel()) break;\n'+anchor)
    write(path,owner)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-retune-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-retune.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,canonical_channel_retune=True,real_roc_on_home=True,additional_peer=False,fixed_endpoint=False,lifetime_seconds=900)
    (OUT/'wmi-retune-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
