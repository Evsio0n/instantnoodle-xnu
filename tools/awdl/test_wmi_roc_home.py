"""Exercise actual staged WMI grant rechecks without hardware or DMA."""
import json
import subprocess
import tempfile
from pathlib import Path
from build_roc import OUT, read

def function(source, name):
    at = source.index(name + '(')
    start = source.rfind('\n', 0, at) + 1
    brace = source.index('{', at)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

receipt = json.loads((OUT / 'wmi-roc-home-build.json').read_text())
source = read(receipt['stage'] + '/wifi/lx/src/live_transport.inc')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#define EG(x) (x)
#define LIVE_WMI_DATA_RING 0xffffu
struct ieee80211_hw { int unused; };
struct sk_buff { int unused; };
struct live_flight { unsigned lease,tag; } flight={11,7};
static struct {unsigned armed,stop,accept,lease;} live={1,0,1,11};
static struct {struct ieee80211_hw *hw;unsigned vdev;} echo;
static struct {unsigned mhz,listen_mhz;} air={5240,5240};
static bool grant,fresh=true,found=true;
static unsigned bound,home_basis=3;
static bool lx_awdl_data_foreign(struct ieee80211_hw *hw){(void)hw;return grant;}
static unsigned lx_awdl_home_basis(struct ieee80211_hw *hw){(void)hw;return home_basis;}
static bool auto_active_fresh(void){return fresh;}
static struct live_flight *live_find_skb(struct ieee80211_hw *hw,struct sk_buff *skb){(void)hw;(void)skb;return found?&flight:NULL;}
static void live_bound(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned v,unsigned ring,unsigned desc,unsigned unused){(void)hw;(void)skb;(void)v;(void)desc;(void)unused;assert(ring==LIVE_WMI_DATA_RING);bound++;}
static void lx_tell(const char *format,...){(void)format;}
'''
code += '\n' + function(source, 'lx_awdl_wmi_data_frequency')
code += '\n' + function(source, 'live_wmi_data_prepare')
code += r'''
int main(void){
 struct ieee80211_hw hw={0};struct sk_buff skb={0};echo.hw=&hw;echo.vdev=1;
 /* A recent home beacon does not substitute for a granted ROC. */
 assert(lx_awdl_wmi_data_frequency(&hw)==0);
 assert(live_wmi_data_prepare(&hw,&skb,1,0,5240)==-ECANCELED);assert(!bound);
 grant=true;home_basis=1;
 assert(lx_awdl_wmi_data_frequency(&hw)==5240);
 assert(live_wmi_data_prepare(&hw,&skb,1,0,5240)==1);assert(bound==1);
 /* A deferred management worker must reject an expired ROC even though
  * the infrastructure channel is still considered recently present. */
 grant=false;home_basis=3;
 assert(lx_awdl_wmi_data_frequency(&hw)==0);
 assert(live_wmi_data_prepare(&hw,&skb,1,1,5240)==-ECANCELED);assert(bound==1);
 grant=true;home_basis=1;
 assert(live_wmi_data_prepare(&hw,&skb,1,1,5745)==-ECANCELED);
 assert(live_wmi_data_prepare(&hw,&skb,2,1,5240)==-ECANCELED);
 flight.lease=12;assert(live_wmi_data_prepare(&hw,&skb,1,1,5240)==-ECANCELED);flight.lease=11;
 fresh=false;assert(live_wmi_data_prepare(&hw,&skb,1,1,5240)==-ECANCELED);fresh=true;
 live.stop=1;assert(lx_awdl_wmi_data_frequency(&hw)==0);
 assert(live_wmi_data_prepare(&hw,&skb,1,1,5240)==-ECANCELED);assert(bound==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-real-grant-') as folder:
    c = Path(folder) / 'test.c'
    binary = Path(folder) / 'test'
    c.write_text(code)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS actual staged grant checks: home-beacon rejection; granted submit; deferred expiry; frequency/vdev/lease/peer/stop rejection without DMA ownership')
