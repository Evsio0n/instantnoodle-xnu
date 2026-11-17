"""Check the actual post-cancel return against the channel backoff contract."""
import json
import re
import subprocess
import tempfile
from pathlib import Path
from build_roc import BUS,OUT,HERE,read

candidate=json.loads((OUT/'wmi-obsolete-retry-build.json').read_text())
source=read(candidate['stage']+'/xnu-deps/build/ath11k/src/ath11k/mac.c')
a=source.index('static int ath11k_mac_op_remain_on_channel(')
b=source.index('\nstatic int ath11k_mac_station_add',a)
assignment=re.search(r'ret = obsolete_window \? -EAGAIN : -ETIMEDOUT;',source[a:b])
assert assignment
code='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include "roc_backoff.inc"
static int completion(bool obsolete_window){int ret;'''+assignment[0]+'''return ret;}
int main(void){
 uint64_t now=1000000;
 int rc=completion(true);assert(rc==-EAGAIN);
 roc_record_outcome(6,false,rc==-ETIMEDOUT,now);
 assert(!roc_recently_unavailable(6,now));
 rc=completion(false);assert(rc==-ETIMEDOUT);
 roc_record_outcome(6,false,rc==-ETIMEDOUT,now);
 assert(roc_recently_unavailable(6,now));
 assert(!roc_recently_unavailable(6,now+16000000));
 roc_record_outcome(6,true,false,now);assert(!roc_recently_unavailable(6,now));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-obsolete-retry-') as folder:
 c=Path(folder)/'test.c';exe=c.with_suffix('');c.write_text(code)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(HERE),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS actual obsolete-cancel return avoids channel ban; genuine timeout backoff/expiry and grant reset retained')
