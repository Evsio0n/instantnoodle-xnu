"""Exercise the staged deferred management gate with actual owner changes."""
import json
import subprocess
import tempfile
from pathlib import Path
from build_roc import OUT,read

r=json.loads((OUT/'wmi-social-owner-build.json').read_text())
s=read(r['stage']+'/wifi/lx/src/live_transport.inc')
a=s.index('static int live_mgmt_prepare(');b=s.index('{',a);end=b+1;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
fn=s[a:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#define EG(x) (x)
#define ES(x,v) ((x)=(v))
struct ieee80211_hw {unsigned n;};
struct lx_radio {struct ieee80211_hw hw;};
struct sk_buff {unsigned n;};
static struct lx_radio radio;
static struct sk_buff frame;
static struct {struct sk_buff *mgmt_frame;struct lx_radio *radio;
 unsigned armed,stop,mgmt_desc,mgmt_bound,mgmt_roc_vdev;} live;
static struct {unsigned vdev;} echo={1};
static struct {unsigned listen_mhz;} air={2437};
static struct {unsigned control_id;} bw={1};
static bool foreign=true;static unsigned home_basis=1;
static struct lx_radio *radio_of(struct ieee80211_hw *h){assert(h==&radio.hw);return &radio;}
static bool lx_awdl_data_foreign(struct ieee80211_hw *h){(void)h;return foreign;}
static unsigned lx_awdl_home_basis(struct ieee80211_hw *h){(void)h;return home_basis;}
'''+fn+r'''
int main(void){
 live.mgmt_frame=&frame;live.radio=&radio;live.armed=1;
 assert(live_mgmt_prepare(&radio.hw,&frame,1,7,2437)==1);
 assert(live.mgmt_bound==1 && live.mgmt_desc==7 && live.mgmt_roc_vdev==1);
 live.mgmt_bound=0;bw.control_id=0;
 assert(live_mgmt_prepare(&radio.hw,&frame,1,8,2437)==-ECANCELED);assert(!live.mgmt_bound);
 bw.control_id=1;foreign=false;
 assert(live_mgmt_prepare(&radio.hw,&frame,1,8,2437)==-ECANCELED);
 foreign=true;assert(live_mgmt_prepare(&radio.hw,&frame,1,8,5745)==-ECANCELED);
 assert(live_mgmt_prepare(&radio.hw,&frame,0,8,2437)==-ECANCELED);
 live.stop=1;assert(live_mgmt_prepare(&radio.hw,&frame,1,8,2437)==-ECANCELED);live.stop=0;
 struct sk_buff stale={0};assert(live_mgmt_prepare(&radio.hw,&stale,1,8,2437)==0);
 /* Preserve the prior management-only idle-home branch. This does not
  * turn a home beacon into an off-channel raw-data grant. */
 foreign=false;home_basis=3;bw.control_id=0;
 assert(live_mgmt_prepare(&radio.hw,&frame,1,9,0)==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-social-owner-') as folder:
    p=Path(folder);(p/'test.c').write_text(code)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS actual staged deferred management: matching owner, wrong owner/expired grant/frequency/vdev/stop rejection, prior home policy preserved')
