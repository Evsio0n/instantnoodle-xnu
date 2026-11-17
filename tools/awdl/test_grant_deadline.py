"""Replay the compiled deadline policy and actual staged ownership accessor."""
import json
import subprocess
import tempfile
from pathlib import Path
from build_roc import OUT,HERE,read

receipt=json.loads((OUT/'wmi-grant-deadline-build.json').read_text())
source=read(receipt['stage']+'/wifi/lx/src/roc.inc')
start=source.index('unsigned long long lx_awdl_latest_grant_us(')
end=source.index('\n}',start)+2
accessor=source[start:end]
code=r'''
#include <assert.h>
#include <stdint.h>
#include "roc_deadline.h"
#define ROC_GET(x) (x)
struct ieee80211_hw {int n;};
struct lx_awdl_roc_state {unsigned active,request_seq;uint64_t latest_grant_us;};
struct lx_radio {struct ieee80211_hw hw;struct lx_awdl_roc_state roc;} radio;
static struct lx_radio *radio_of(struct ieee80211_hw *hw){assert(hw==&radio.hw);return &radio;}
'''+accessor+r'''
int main(void){
 assert(awdl_latest_grant(1000000,0)==1200000);
 assert(awdl_latest_grant(1000000,1100000)==1095904);
 assert(awdl_latest_grant(1000000,1400000)==1200000);
 assert(awdl_latest_grant(1000000,999999)==1200000);
 assert(awdl_latest_grant(1000000,1004096)==1200000);
 assert(awdl_latest_grant(UINT64_MAX-1000,0)==UINT64_MAX);
 radio.roc=(struct lx_awdl_roc_state){1,7,1095904};
 assert(lx_awdl_latest_grant_us(&radio.hw,7)==1095904);
 assert(!lx_awdl_latest_grant_us(&radio.hw,6));
 assert(!lx_awdl_latest_grant_us(&radio.hw,0));
 radio.roc.active=0;assert(!lx_awdl_latest_grant_us(&radio.hw,7));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-deadline-') as folder:
 c=Path(folder)/'test.c';exe=Path(folder)/'test';c.write_text(code)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(HERE),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS staged token/active ownership; rendezvous cutoff, fallback, cap, stale plan and integer boundary')
