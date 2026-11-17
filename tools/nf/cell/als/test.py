#!/usr/bin/env python3
import subprocess
import tempfile
import shutil
from pathlib import Path
import json
import re
import struct
from build import HERE, SOURCE, K, digest
sources = json.loads((HERE / 'sources.json').read_text())
with tempfile.TemporaryDirectory() as temp:
    t=Path(temp)
    for name, expected in sources.items():
        file=SOURCE/name
        if name=='MiniAppleARMBacklight.cpp': file=K/'xnu/files/iokit/Kernel'/name
        if name=='slpi_dma_route.h': file=K/'xnu/files/bsd/dev'/name
        if name=='hk_adsp.c': file=HERE.parent/'overlay/xnu/files/bsd/dev'/name
        assert digest(file)==expected, name
        shutil.copyfile(file,t/name)
    for patch in ['sensor.patch','display.patch']:
        subprocess.run(['patch','-s','-p1','-i',str(HERE/patch)],cwd=t,check=True)
    subprocess.run(['c++','-std=c++20','-I',str(t),str(HERE/'test.cpp'),'-o',str(t/'test')],check=True)
    subprocess.run([str(t/'test')],check=True)
    subprocess.run(['cc','-std=c11','-O2','-I',str(t),'-I',str(K/'xnu/files/bsd/dev'),
                    str(HERE/'test_stream_lifetime.c'),'-o',str(t/'test_lifetime')],check=True)
    subprocess.run([str(t/'test_lifetime')],check=True)
    log = K / 'logs/074253-run.whole.log'
    if log.is_file():
        rows = re.findall(r"slpi-wise: sample=(\d+) ticks=(\d+) status=(\d+) n=\d+ lux_bits=([a-fA-F0-9]+)", log.read_text(errors="replace"))
        subprocess.run([str(t/'test'), '--replay'], input=''.join(' '.join(row)+'\n' for row in rows), text=True, check=True)
    for run,expected in [('072907',True),('221312',False)]:
        log=K/'logs'/(run+'-run.whole.log')
        if not log.is_file() and run=='221312':
            log=K/'out/cell-als-stock-full.txt'
        if not log.is_file():continue
        rows=re.findall(r'sets DisplayBrightness = .*?<key>Brightness</key><integer[^>]+>0x([a-f0-9]+)',log.read_text(errors='replace'))
        assert len(rows)>=15, (run,len(rows))
        out=subprocess.check_output([str(t/'test'),'--slider'],input=''.join(v+' 1\n' for v in rows),text=True).split()
        # Returned whole logs sometimes contain both the plain and timestamped copy.
        assert int(out[0])==len(rows) and (int(out[1])>0)==expected,(run,out)
        print(f'Run {run}: {out[0]} stock slider reports, {out[1]} accepted changes')
    # Backboard's per-update values cover the end of the fade after the boot
    # display driver's first 80 bounded debug prints. The final zero follows
    # the panel's actual confirmed DCS-off in the same chronological trace.
    for run,expected in [('072907',True),('222029',False)]:
        log=K/'logs'/(run+'-run.whole.log')
        if not log.is_file() and run=='222029':
            log=K/'out/cell-als-candidate-full.txt'
            if not log.is_file(): log=K/'logs/222029-run.log.live'
        if not log.is_file():continue
        lit=1;raw=[];before=[];after=[]
        for line in log.read_text(errors='replace').splitlines():
            if 'display: the panel dark (' in line:lit=0
            elif 'display: the panel lit (' in line:lit=1
            m=re.search(r'brightness change:([0-9.]+) reason:BrightnessSystemDidChange',line)
            if m:
                value=float(m[1])
                raw.append(f'{struct.pack(">d",value).hex()} {lit}\n')
                (before if lit else after).append(value)
        assert len(raw)>5
        result=subprocess.check_output([str(t/'test'),'--slider'],input=''.join(raw),text=True).split()
        assert (int(result[1])>0)==expected,(run,result)
        if not expected:
            assert min(before)>=0.49902 and max(before)<=0.50098 and 0.0 in after
        print(f'Run {run}: complete OS callbacks {len(raw)}, normalized changes while lit {result[1]}, dark callbacks {len(after)}')
    # Timeline of actual original native submissions and their qualified
    # goals. Ring files overlap; pair only same sequence/tick within a snapshot.
    tag='072309'
    logs=list((K/'logs').glob(tag+'-run.log.live.[0-9]*'))+[K/'logs'/(tag+'-run.log.live'),K/'out/cell-als-exposure-full.txt']
    rows={}
    for log in logs:
        if not log.is_file():continue
        contents=log.read_text(errors='replace')
        goals={ (n,t):int(goal) for n,t,goal in re.findall(r'light-curve: seq=(\d+) tick=(\d+) mlux=\d+ nits_x10000=\d+ reference_dbv=\d+ goal_dbv=(\d+)',contents) }
        for n,tick,us,base,target in re.findall(r'light-panel: select seq=(\d+) tick=(\d+) us=(\d+) lux_bits=[a-f0-9]+ band=\d+ from=(\d+) target=(\d+)',contents):
            if (n,tick) in goals:rows[int(us)]=(int(n),int(us),int(base),int(target),goals[n,tick])
    if rows:
        text=''.join(' '.join(map(str,rows[us]))+'\n' for us in sorted(rows))
        subprocess.run([str(t/'test'),'--goals'],input=text,text=True,check=True)
