"""Exercise the actual staged encoder and compare every TLV body to control."""
import json
from pathlib import Path
import subprocess
import tempfile
from build_roc import HERE,OUT,read

receipt=json.loads((OUT/'wmi-tlv-order-build.json').read_text())
candidate=read(receipt['stage']+'/wifi/lx/src/awdl_discovery.h')
control=read('/tmp/awdl-group-deadline-pnUpu4/wifi/lx/src/awdl_discovery.h')
emit=r'''
#include <stdio.h>
#include "awdl_discovery.h"
int main(int argc,char **argv){
 (void)argv;
 uint8_t mac[6]={2,0,0,0,0,7},out[LX_AWDL_DISCOVERY_MAX];
 struct lx_awdl_discovery_window w={.anchor_us=1000,.now_us=1000+65536+9216,
 .grant_end_us=1000+5*65536,.ieee_sequence=17,.af_period_tu=110,.channel=48,
 .dualband_supported=true};
 struct lx_awdl_discovery_ht ht={.capabilities=0x12,.rx_mcs0=255};
 size_t n=lx_awdl_discovery_action(out,sizeof(out),mac,argc==2?0:3,&w,argc==4?&ht:NULL);
 return n && fwrite(out,1,n,stdout)==n?0:1;
}
'''

def tlvs(frame):
    at=40
    result=[]
    while at<len(frame):
        assert at+3<=len(frame)
        size=int.from_bytes(frame[at+1:at+3],'little')
        end=at+3+size
        assert end<=len(frame)
        result.append((frame[at],frame[at+3:end]))
        at=end
    return result

with tempfile.TemporaryDirectory(prefix='awdl-tlv-order-') as folder:
    folder=Path(folder)
    outputs={}
    for name,header in [('control',control),('candidate',candidate)]:
        stage=folder/name;stage.mkdir()
        (stage/'awdl_discovery.h').write_text(header)
        (stage/'emit.c').write_text(emit)
        cc=['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(HERE)]
        subprocess.run(cc+[str(stage/'emit.c'),'-o',str(stage/'emit')],check=True)
        outputs[name]=[subprocess.check_output([str(stage/'emit')]+['x']*n) for n in (1,2,3)]
    for old,new in zip(outputs['control'],outputs['candidate']):
        assert len(old)==len(new) and old[:40]==new[:40]
        before,after=tlvs(old),tlvs(new)
        assert [t[0] for t in before[:2]]==[4,5]
        assert [t[0] for t in after[:2]]==[4,6]
        assert sorted(before)==sorted(after), 'a TLV body changed'
    test=(HERE/'test_discovery.c').read_text().replace('{4,5,18,24,6}','{4,6,5,18,24}')
    stage=folder/'candidate';(stage/'test.c').write_text(test)
    subprocess.run(cc+[str(stage/'test.c'),'-o',str(stage/'test')],check=True)
    subprocess.run([str(stage/'test')],check=True)
print('PASS actual staged PSF/MIF: first4/6, same complete TLV bodies and headers, typed roundtrip and grant bounds')
