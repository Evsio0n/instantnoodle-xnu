"""Replay actual staged latency history through consecutive missed windows."""
import json
import subprocess
import tempfile
from pathlib import Path
from build_roc import OUT,read
r=json.loads((OUT/'wmi-censored-lead-build.json').read_text())
s=read(r['stage']+'/wifi/lx/src/roc.inc')
a=s.index('static struct { unsigned samples[5],count,next; } latencies[178];')
b=s.index('struct slot_target {',a)
policy=read(r['stage']+'/wifi/lx/src/roc_deadline.h')
code='''#include <stdint.h>
#include <assert.h>
#include "roc_deadline.h"
'''+s[a:b]+'''
int main(void){
 unsigned ch=48;slot_sample(ch,7706);assert(slot_lead(ch)==7706);
 for(unsigned i=0;i<6;i++){
  unsigned previous=slot_lead(ch);
  uint64_t now=1000000,planned_end=now+previous+49152;
  uint64_t latest=awdl_latest_grant(now,planned_end);
  assert(latest==planned_end-4096); /* lead may exceed the unplanned 200 ms cap */
  unsigned lower=(unsigned)(latest-now);
  slot_sample(ch,lower+8192);
  unsigned next=slot_lead(ch);
  assert(next>=previous && next<=250000);
  if(previous<250000)assert(next>previous);
 }
 assert(slot_lead(ch)==250000);
 for(unsigned i=0;i<5;i++)slot_sample(ch,7706);
 assert(slot_lead(ch)==7706); /* slow-mode protection ages out on real grants */
 slot_sample(ch,3000000);assert(slot_lead(ch)==250000);
 assert(awdl_latest_grant(1000000,0)==1200000);
 assert(awdl_latest_grant(UINT64_MAX-1000,0)==UINT64_MAX);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-censored-lead-') as d:
 p=Path(d);(p/'roc_deadline.h').write_text(policy);(p/'test.c').write_text(code)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',d,str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS staged censored latency history grows advance, retains planned budget, caps at 250 ms, ages out on five real grants')
