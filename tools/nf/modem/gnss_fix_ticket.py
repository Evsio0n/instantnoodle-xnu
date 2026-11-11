"""Verify the pinned private GNSS profile. --run schedules one consent-preserving RAM run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/sensors'))
import cold_recover

MANIFEST = Path(__file__).with_name('gnss_fix_profile.json')
LIMIT = 340
ARGS = ('fb_panel=amb655uv01 fb_hz=90 cpus=8 bootcpu=7 gpu=1 board_gpu_zap=0x8e21a000 '
        'wifi=0 adsp=0 bt=0 sound=0 sound_amps=0 haptic=0 cam=0 torch=0 syssleep=0 '
        'ufs=2 ufs_write=0 root_apfs=ufs_darwin hk_data=2 hk_data_mb=1024 var_store=0 '
        'var_data=hfs var_data_format=1 modem=3 modem_mission=1 modem_radio=0 modem_loc=1 modem_gnss=2')
QUALITY = re.compile(rb'gnss-fix-quality seq=(\d+) state=(\d+) valid_count=(\d+) age_ms=(\d+) end')

def spec(manifest=None, root_stem='gnss-measure'):
    m = json.loads((manifest or MANIFEST).read_text())
    kernel = ROOT / 'out/kernels' / f'nf-gnss-fix-{m["kernel_sha256"]}.mach'
    ram = ROOT / 'out' / f'{root_stem}-{m["root_sha256"]}.apfs'
    for path, expected in ((kernel, m['kernel_sha256']), (ram, m['root_sha256']), (ROOT/'userland/flashboot.apfs', m['resident_sha256'])):
        with path.open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                raise ValueError('changed artifact ' + path.name)
    args = ARGS.split()
    for key, expected in [('wifi','0'),('modem_gnss','2'),('modem_loc','1'),('modem_mission','1'),('hk_data','2'),('ufs_write','0'),('var_store','0')]:
        if [x for x in args if x.startswith(key+'=')] != [key+'='+expected]:
            raise ValueError('unsafe or contradictory option '+key)
    prefix = (ROOT.parent/'oneplus8-audit/gpu-plan/observed-loader-prefix.txt').read_bytes()
    n = len(prefix) + len(f'loader_return_after={LIMIT} epoch=0000000000 sb_delay=8 {ARGS}')
    mac = ROOT/'fw-wifi/mac'
    if mac.exists(): n += len(' wifi_mac=') + len(b''.join(mac.read_bytes().split()))
    if n > 607: raise ValueError('kernel command line too long')
    return m,kernel,ram,n

def evidence(files, manifest):
    facts = dict(bundle_visible=False,launch_accepted=False,app_foreground=False,owner_tapped=False,
                core_request=False,core_reply=False,loc_accepted=False,teardown=False,pins_down=False)
    checks = {'bundle_visible': b'GNSSMonitor bundle visible under /Applications on the RAM profile',
              'launch_accepted': b'battery_test: SpringBoard was asked to launch com.hackios.gnss-monitor: it does',
              'app_foreground': b'GNSSMonitor: private-fix profile active;',
              'owner_tapped': b'GNSSMonitor: foreground location check requested by user',
              'core_request': b'GNSSMonitor: authorized public requestLocation issued',
              'core_reply': b'GNSSMonitor: fresh public CLLocation reply;',
              'loc_accepted': b'modem: qmi loc: generic GNSS start accepted',
              'teardown': b'modem: level 3 is done',
              'pins_down': b'modem: a second after the last reset: MDM2AP status 0, errfatal 0; AP2MDM status 0; PERST 0'}
    samples = {}
    ordered_public = False
    expected_bundle = f'gnss-bundle-sha executable={manifest["executable_sha256"]} plist={manifest["plist_sha256"]} end'.encode()
    facts.update(bundle_matches=False,scene_foreground=False)
    for f in files:
        buf=f.read_bytes()
        facts['bundle_matches'] |= expected_bundle in buf
        facts['scene_foreground'] |= bool(re.search(rb'new scene host settings:[^\r\n]{0,100}<sceneID:com\.hackios\.gnss-monitor[^>]{0,100}> [^\r\n]{0,30}-> foreground',buf))
        for k, check in checks.items():
            if k=='bundle_visible':
                facts[k] |= b'[ ok ] ' + check in buf
            else: facts[k] |= check in buf
        # The same app process must emit a tap, authorized request and fresh reply
        # in order. Saved windows overlap, so one complete sequence is sufficient.
        progress={}
        for line in buf.splitlines():
            if b'GNSSMonitor:' not in line:continue
            caller=re.search(rb'\bGNSSMonitor\[(\d+):[^\]]+\]',line)
            if not caller:continue
            pid=caller[1]
            if checks['owner_tapped'] in line:progress[pid]=1
            elif checks['core_request'] in line and progress.get(pid)==1:progress[pid]=2
            elif checks['core_reply'] in line and progress.get(pid)==2:ordered_public=True
        for m in QUALITY.finditer(buf):
            seq,state,count,age=map(int,m.groups())
            if not 0<=seq<48 or not 0<=state<=9 or (state==2 and (count<1 or age>15000)):
                raise ValueError('invalid coordinate-free fix state or age')
            if seq in samples and samples[seq] != (state,count,age):
                raise ValueError('conflicting measurement sample')
            samples[seq]=state,count,age
    facts.update(public_reply_sequence=ordered_public,quality_samples=len(samples),satellite_fix_seen=any(s==2 and n>0 and age<=15000 for s,n,age in samples.values()),
                 observed_states=sorted({s for s,_,_ in samples.values()}),environment='unclassified')
    return facts

def inside(m,kernel,ram, *, folder_prefix='gnss-fix', collect=evidence, evaluate=None,
           run_args=ARGS, seconds=LIMIT, done='modem: level 3 is done', finish_in_stock=False, single_cold_boundary=False):
    ticket=os.environ.get('BOOTQ_TICKET','')
    if not ticket.isdigit():raise RuntimeError('bootq ownership required')
    dest=ROOT/'out/eye'/f'{folder_prefix}-ticket-{ticket}'
    dest.mkdir(mode=0o700,parents=True,exist_ok=False)
    if not single_cold_boundary and cold_recover.LOADER not in cold_recover.devices():cold_recover.recover(True,False,120)
    cold_recover.recover(True,False,120)
    env={k:v for k,v in os.environ.items() if not k.startswith('XL_')}
    env.update(PATH='/opt/homebrew/opt/coreutils/libexec/gnubin:'+env['PATH'], XL_CMDLINE=run_args, XL_SECS=str(seconds), XL_SB_DELAY='8',
               XL_KERNEL=str(kernel), XL_RAMDISK=str(ram), XL_FOLLOW='2', XL_FOLLOW_KEEP='1', XL_DONE=done)
    began=time.time()
    try:
        with (dest/'xl.txt').open('x') as out:
            print(f'gnss-profile ticket={ticket} kernel={m["kernel_sha256"]} root={m["root_sha256"]} app={m["bundle_cdhash"]}',file=out,flush=True)
            rc=subprocess.run(['timeout','--kill-after=15',str(seconds+175),'bash','-ec',
                '. common/lib.sh; load_device instantnoodle; ./xl wait 1; ./xl ramdisk userland/flashboot.apfs; ./xl run'],
                cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT).returncode
        transcript=(dest/'xl.txt').read_text(errors='replace')
        ids=re.findall(r'\[logs/(\d{6})-run.log\]',transcript)
        if not ids or len(set(ids))!=1:raise RuntimeError('no unique run ID; '+str(dest))
        rid=ids[0]
        files=list((ROOT/'logs').glob(rid+'-run.log.live.*'))+[ROOT/'logs'/f'{rid}-run.log.live',ROOT/'logs'/f'{rid}-run.log']
        files=[f for f in files if f.is_file() and f.stat().st_mtime>=began-1]
        f=collect(files,m)
        f.update(ticket=int(ticket),run=rid,kernel=m['kernel_sha256'],ramdisk=m['root_sha256'],app_cdhash=m['bundle_cdhash'],
                 clean_return=not rc and bool(re.search(r'result: returned \d+ 0\s+\[logs/',transcript)))
        f['measurement_ready']=(all(f[k] for k in ('bundle_visible','bundle_matches','launch_accepted','loc_accepted','teardown','pins_down','quality_samples','clean_return'))
                                and (f['app_foreground'] or f['scene_foreground']))
        if evaluate is not None:
            f['measurement_ready']=evaluate(f)
        (dest/'evidence.json').write_text(json.dumps(f,indent=2)+'\n')
        print(dest/'evidence.json',flush=True)
        return 0 if f['measurement_ready'] else 1
    finally:
        if finish_in_stock:
            events=[]
            def witness(stage,serial):
                events.append({"stage":stage,"endpoint":"stock" if serial==cold_recover.STOCK else "developer","wall":time.time()})
            cold_recover.recover(True,True,120,witness=witness)
            if not events or events[-1]['stage']!='stock' or events[-1]['endpoint']!='stock':
                raise RuntimeError('no verified stock FASTBOOT witness for handoff')
            (dest/'stock-handoff.json').write_text(json.dumps({'ticket':int(ticket),'product':'kona','events':events},indent=2)+'\n')
        else:
            cold_recover.recover(True,False,120)

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    group=p.add_mutually_exclusive_group()
    group.add_argument('--run',action='store_true');group.add_argument('--inside-ticket',action='store_true')
    a=p.parse_args()
    if a.run:
        return subprocess.run([sys.executable,str(Path(__file__).with_name('gnss_settings_ticket.py')),'--run'],cwd=ROOT).returncode
    if a.inside_ticket:
        raise RuntimeError('retired single-directory GNSS image; use gnss_settings_ticket.py')
    m,k,r,n=spec()
    print('GNSS profile',m['kernel_sha256'][:12],m['root_sha256'][:12],n,'argument bytes',flush=True)
    return 0
if __name__=='__main__':sys.exit(main())
