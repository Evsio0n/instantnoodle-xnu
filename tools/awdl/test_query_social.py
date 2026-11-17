"""Run the staged social-window gate and deferred frequency selection."""
import json, subprocess, tempfile
from pathlib import Path
from build_roc import OUT,read
r=json.loads((OUT/'wmi-query-social-build.json').read_text())
s=read(r['stage']+'/wifi/lx/src/auto_owner.inc')
a=s.index('if (lx_awdl_data_foreign(&r->hw) && auto_channel(r,channel,true)) {')
b=s.index('\n\t\tlx_pause_ms(2);',a)
gate=s[a:b]
s=read(r['stage']+'/wifi/lx/src/live_transport.inc')
a=s.index('unsigned lx_awdl_wmi_data_frequency(');b=s.index('\nstatic int live_wmi_data_prepare',a)
freq=s[a:b]
code=r'''
#include <assert.h>
#include <stdbool.h>
#define EG(x) (x)
struct ieee80211_hw {int unused;};
struct ieee80211_vif {int unused;};
struct lx_radio {struct ieee80211_hw hw;};
static struct {unsigned armed,stop;}live={1,0};
static struct {struct ieee80211_hw *hw;unsigned vdev;}echo;
static struct {unsigned active,prepeer,mhz,listen_mhz;}air={1,0,5240,2437};
static struct {struct ieee80211_vif *vif;unsigned armed,control_id,own_mhz;}bw;
static bool grant=true,usable=true;
static unsigned data,discovery;
static bool lx_awdl_data_foreign(struct ieee80211_hw *h){(void)h;return grant;}
static unsigned lx_awdl_home_basis(struct ieee80211_hw *h){(void)h;return 1;}
static bool auto_channel(struct lx_radio *r,unsigned ch,bool tx){(void)r;(void)ch;assert(tx);return usable;}
static void live_discovery(struct lx_radio *r,struct ieee80211_vif *v,void *ch){(void)r;(void)v;(void)ch;discovery++;}
static void live_data(struct lx_radio *r,struct ieee80211_vif *v,void *ch,unsigned basis){(void)r;(void)v;(void)ch;assert(basis==1);data++;}
'''+freq+'\nstatic void service(struct lx_radio *r,struct ieee80211_vif *v,void *ch,unsigned channel){'+gate+r'''
}
int main(void){
 struct lx_radio radio={0};struct ieee80211_vif vif={0},wrong={0};
 echo.hw=&radio.hw;echo.vdev=1;bw.vif=&vif;bw.armed=1;bw.control_id=1;bw.own_mhz=2437;
 service(&radio,&vif,0,6);assert(data==1 && discovery==1);
 assert(lx_awdl_wmi_data_frequency(&radio.hw)==2437);
 grant=false;service(&radio,&vif,0,6);assert(data==1 && discovery==1);assert(lx_awdl_wmi_data_frequency(&radio.hw)==0);
 grant=true;bw.control_id=0;service(&radio,&vif,0,6);assert(data==1);assert(lx_awdl_wmi_data_frequency(&radio.hw)==2437);
 bw.control_id=1;bw.vif=&wrong;service(&radio,&vif,0,6);assert(data==1);
 bw.vif=&vif;air.prepeer=1;service(&radio,&vif,0,6);assert(data==1);
 air.prepeer=0;air.active=0;service(&radio,&vif,0,6);assert(data==1);
 air.active=1;usable=false;service(&radio,&vif,0,6);assert(data==1);
 live.stop=1;assert(lx_awdl_wmi_data_frequency(&radio.hw)==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS staged social grant data path: real grant/owner/peer gates, social frequency, expired grant/stop rejection')
