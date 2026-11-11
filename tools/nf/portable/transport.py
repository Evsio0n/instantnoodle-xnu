"""One bounded IN2010 RAM boot with an exclusive queue hold across physical USB absence."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import sys
import tempfile
import time
from build_loader import ROOT,HERE,OUTPUT,need

sys.path[:0]=[str(ROOT/'tools/sensors'),str(ROOT/'tools')]
import cold_recover as cold
import quarantine_state as qs
import recover_hold as recovery
from iphont.device import payload

LOADER_SHA='f460cdc64693f898712593f74e6dfabeefa65d89a7d58054e2a22eb7aa44ecd1'
COMMON=('fb_panel=amb655uv01 fb_hz=90 cpus=8 bootcpu=7 gpu=1 board_gpu_zap=0x8e21a000 '
        'wifi=0 adsp=0 sound=0 sound_amps=0 syssleep=0 '
        'ufs=2 ufs_write=0 root_apfs=ufs_darwin hk_data=2 hk_data_mb=1024 var_store=0 '
        'var_data=hfs var_data_format=1')
TIMERS={'rehearsal':300,'gnss':1200}
SEP=b'\n== KERNEL LOG ==\n'
LIVE=b'\n== KERNEL LOG (the kernel is still running) ==\n'
END=b'\n== END OF LOG ==\n'
UNSAFE=(b'THE KERNEL HUNG',b'THE KERNEL LEFT CORE',b'cold recovery required',b'EARLY KERNEL EXCEPTION',
        b'THE WARM RESET FAILED',b'panic(cpu',b'bytes written to logdump')
PINS=b'modem: a second after the last reset: MDM2AP status 0, errfatal 0; AP2MDM status 0; PERST 0,'


def cmdline(mode,token,epoch):
    need(re.fullmatch('[a-f0-9]{32}',token) and mode in TIMERS,'invalid mode/session')
    levels=('modem=0 modem_mission=0 modem_radio=0 modem_loc=0 modem_gnss=0' if mode=='rehearsal'
            else 'modem=3 modem_mission=1 modem_radio=0 modem_loc=1 modem_gnss=2')
    return f'loader_return_after={TIMERS[mode]} epoch={epoch} sb_delay=8 {COMMON} {levels} portable_id={token}'


def artifacts():
    m=json.loads((HERE/'profile.json').read_text())
    result={}
    for key,path,digest in (
        ('image',OUTPUT/f'loader-{LOADER_SHA}.img',LOADER_SHA),
        ('kernel',OUTPUT/f'portable-{m["mach"]}.mach',m['mach']),
        ('root',OUTPUT/f'portable-{m["apfs"]}.apfs',m['apfs']),
        ('resident',ROOT/'userland/flashboot.apfs',m['resident'])):
        with path.open('rb') as f:
            need(hashlib.file_digest(f,'sha256').hexdigest()==digest,'private artifact changed: '+key)
        result[key]=path
    return result


def info(raw,result,session):
    """Extract only anchored fields from the current log and verify final framing by length."""
    nonce=session['id'].encode()
    need(len(raw)<=2<<20 and raw.startswith(b'== XNULOADER run (usb) =='),'wrong log')
    if any(x in raw for x in UNSAFE):raise ValueError('unsafe kernel or loader result')
    if re.fullmatch(r'running [0-9]+ 0', result):
        prefix,marker,body=raw.partition(LIVE);final=False
    else:
        r=re.fullmatch(r'returned ([0-9]+) 0',result)
        need(r,'unexpected loader result')
        prefix,marker,body=raw.partition(SEP); final=True
        size=min(int(r[1]),(2<<20)-len(prefix)-8192)
        need(size>0 and body[size:]==END,'incomplete returned log')
        body=body[:size]
    need(marker,'missing live/returned kernel log')
    lines=re.findall(rb'(?m)^cmdline: ([^\r\n]+)$',prefix)
    need(len(lines)==1 and lines[0].endswith(b' '+session['cmdline'].encode()) and
         lines[0].split().count(b'board=instantnoodle')==1,'wrong or truncated boot session')
    ready=re.search(rb'(?m)^portable-ready-v1 id='+nonce+rb' end$',body) is not None
    rehearsal=re.search(rb'(?m)^portable-rehearsal-v1 id='+nonce+rb' completed=1 end$',body) is not None
    rows=set(re.findall(rb'(?m)^portable-gnss-v1 id='+nonce+rb' started=([01]) complete=([01]) reports=([0-9]{1,10}) fix=([01]) end$',body))
    need(len(rows)<=1,'conflicting result lines')
    receipt=None
    if rows:
        started,complete,reports,fix=map(int,next(iter(rows)))
        need(reports<=2147483647 and (not complete or started) and (not fix or (started and reports)),
             'contradictory result fields')
        receipt=dict(started=bool(started),complete=bool(complete),reports=reports,fix=bool(fix))
    if session['mode']=='rehearsal':
        need(not rows and b'modem: modem=3:' not in body,'modem activity in rehearsal')
    else:need(not rehearsal,'rehearsal evidence in GNSS mode')
    cable_rows=sorted(set((int(plug),int(event)) for plug,event in re.findall(
        rb'(?m)^portable-usb-v2 id='+nonce+rb' physical=(-1|0|1) usb_event=([01]) end$',body)))
    control_rows=sorted(set(tuple(map(int,row)) for row in re.findall(rb'(?m)^portable-control-v2 id='+nonce+
        rb' action=([123]) state=([0-8]) physical=(-1|0|1) event=([01]) errno=([0-9]{1,3}) end$',body)))
    http_rows=sorted(set((int(action),reason.decode(),int(code)) for action,reason,code in re.findall(
        rb'(?m)^portable-http-v2 action=([123]) outcome=(kernel|origin|token|request) code=([0-9]{1,3}) end$',body)))
    safari_requested=(b'battery_test:' in body and b'http://127.0.0.1:8765/' in body)
    return dict(cable_rows=cable_rows,control_rows=control_rows,http_rows=http_rows,safari_requested=safari_requested,ready=ready,rehearsal=rehearsal,receipt=receipt,final=final,
                teardown=bool(re.search(rb'(?m)^portable-teardown-v1 id='+nonce+
                    rb' loc_stop=1 polling_exited=1 warm_reset=1 pins_low=1 end$',body)) or
                    (PINS in body and b'THE WARM RESET IS DONE' in body and b'modem: level 3 is done' in body),
                timed=b'--- loader_return_after expired ---' in body,
                host_return=b'--- the host asked for the end of the run: back to the loader ---' in body)


def fb(serial,*args,limit=9):
    return cold.fastboot('-s',serial,*map(str,args),timeout=limit)


def endpoints():
    raw=cold.fastboot('devices')
    values=[row[0] for line in raw.splitlines() if len(row:=line.split())>=2 and row[1]=='fastboot']
    need(len(values)<=1 and set(values)<={cold.STOCK,cold.LOADER},'unexpected or ambiguous USB targets')
    return values[0] if values else None


def sample(s,decoder=info):
    # The exclusive .lock covers the stage and get_staged pair. Keep all bytes in a
    # subprocess pipe; even an interrupted host has no raw spatial data on disk.
    before=recovery.mac_usb_session(cold.LOADER)
    need(before['locationID']==s['stock']['locationID'],'wrong USB port')
    need(cold.variable(cold.LOADER,'portable')=='ram-log-v1' and
         cold.variable(cold.LOADER,'board')=='instantnoodle','wrong developer loader')
    result=cold.variable(cold.LOADER,'result')
    if not result.startswith(('returned ','running ')):raise ValueError('unexpected loader state: '+result[:24])
    fb(cold.LOADER,'oem','portable-log')
    cmd=['fastboot','-s',cold.LOADER,'get_staged','/dev/stdout']
    r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,check=True,timeout=12)
    # A live run may finish between the two reads; wait for a fresh whole pair.
    if result!=cold.variable(cold.LOADER,'result'):return None
    need(recovery.mac_usb_session(cold.LOADER)==before,'USB changed during snapshot')
    return before,decoder(r.stdout,result,s)


def update(s,**changes):
    s.update(changes)
    qs.atomic(Path(s['folder'])/'session.json',json.dumps(s,indent=2)+'\n')


def observe(s,seconds,*,seen=endpoints,read=sample,clock=time.monotonic,pause=time.sleep,save=update,
            end_run=lambda:fb(cold.LOADER,'oem','stop')):
    deadline=clock()+seconds
    failures=0
    while clock()<deadline:
        try:
            endpoint=seen()
            if endpoint is None:
                if s.get('ready') and not s.get('absent'):
                    save(s,absent=True);print('已观察到 USB 断开，队列保留本次手机。',flush=True)
            else:
                need(endpoint==cold.LOADER,'stock or unrelated endpoint during observation')
                item=read(s)
                if item:
                    usb,evidence=item
                    if not s.get('matched_kernel'):
                        save(s,matched_kernel=True)
                        print('已核对本次内核和启动标识，正在等手机页面。',flush=True)
                    if evidence['safari_requested'] and not s.get('safari_requested'):
                        save(s,safari_requested=True)
                        print('本次内核已记录打开本机 Safari 页面请求。',flush=True)
                    fresh=recovery.fresh(s['developer'],usb)
                    if not s.get('absent') and fresh:
                        # Re-enumeration without witnessing absence cannot prove an unplug.
                        raise ValueError('unwitnessed change of developer USB session')
                    need((s.get('absent') and fresh) or usb==s['developer'],'unexpected USB identity')
                    if evidence['ready'] and not s.get('ready'):
                        save(s,ready=True)
                        print('拔线演练已准备：可拔线，留在电脑旁点开始。不会启动定位。'
                              if s['mode']=='rehearsal' else '本次户外 GNSS 已准备：可拔线，到室外后再点开始。',flush=True)
                    if s.get('absent') and fresh and not s.get('reconnected'):
                        save(s,reconnected=True);print('已匹配原端口、本次内核运行标识和新的 USB 会话。',flush=True)
                    if evidence['receipt'] is not None:
                        need(s.get('receipt') in (None,evidence['receipt']),'result changed across snapshots')
                    changes={k:v for k,v in evidence.items() if k in ('rehearsal','timed','teardown','host_return') and v and not s.get(k)}
                    if evidence['receipt'] is not None and not s.get('receipt'):changes['receipt']=evidence['receipt']
                    for key in ('cable_rows','control_rows','http_rows'):
                        merged=sorted({tuple(row) for row in s.get(key,[])}|set(evidence[key]))
                        if merged!=[tuple(row) for row in s.get(key,[])]:changes[key]=merged
                    if changes:save(s,**changes)
                    if (s.get('reconnected') and not evidence['final'] and not s.get('stop_requested')
                            and (s.get('rehearsal') or (s.get('teardown') and (s.get('receipt') or {}).get('complete')))):
                        save(s,stop_requested=True)
                        end_run()
                    if evidence['final']:
                        save(s,returned=True)
                        return
        except (cold.RecoveryError,subprocess.SubprocessError) as err:
            failures+=1
            if failures in (3,15):
                print('正在等待可验证的 USB 快照，传输状态 '+type(err).__name__,flush=True)
            # Transport failure is not absence, proof of return, or a reason to boot/reset.
            pass
        pause(min(2,max(0,deadline-clock())))
    raise TimeoutError('bounded observation ended; hold retained. Reconnect and resume collection')


def complete(s):
    if not all(s.get(k) for k in ('ready','absent','reconnected','returned')):return False
    if not (s.get('timed') or (s.get('stop_requested') and s.get('host_return'))):return False
    if s.get('profile') and not any(row[0]==0 for row in s.get('cable_rows',[])):return False
    if s['mode']=='rehearsal':return bool(s.get('rehearsal'))
    r=s.get('receipt') or {}
    return bool(s.get('teardown') and r.get('started') and r.get('complete'))


@contextlib.contextmanager
def hardware_locks(b):
    with contextlib.ExitStack() as stack:
        for suffix in ('.run','.lock'):
            handle=stack.enter_context(open(b.PHONE+suffix,'a'))
            fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
        yield


def stock():
    need(endpoints()==cold.STOCK and cold.variable(cold.STOCK,'product')=='kona','one stock IN2010 FASTBOOT endpoint required')
    return recovery.mac_usb_session(cold.STOCK)


def wait_for(target,seconds):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        if endpoints()==target:return
        time.sleep(1)
    raise TimeoutError('expected USB endpoint did not appear')


def current(b,s):
    h=b.quarantine()
    need(h and h.get('portable_id')==s['id'] and h.get('ticket')==s['ticket'],'portable queue reservation changed')
    return h


def finish(b,s):
    # Results are saved before any reset, including a legitimate no-fix result.
    # All other paths keep the durable bootq quarantine for a verified cold recovery.
    good=complete(s)
    if s['mode']=='gnss' and not s.get('teardown'):
        raise ValueError('modem shutdown not independently verified; cold recovery required')
    if not s.get('reset_pending'):
        item=sample(s)
        need(item and item[1]['final'],'final result unavailable on expected device')
        update(s,reset_pending=True,reset_from=item[0])
        try:fb(cold.LOADER,'reboot','bootloader')
        except (cold.RecoveryError,subprocess.TimeoutExpired):pass
    wait_for(cold.STOCK,115)
    final_stock=stock()
    need(recovery.fresh(s['reset_from'],final_stock) and recovery.fresh(s['stock'],final_stock),'stock reset unverified')
    update(s,stock_after=final_stock)
    result=dict(ticket=s['ticket'],mode=s['mode'],id=s['id'],matched_detach=bool(s.get('reconnected')),
                timed_return=bool(s.get('timed')),verified_host_return=bool(s.get('stop_requested') and s.get('host_return')),profile=s.get('profile'),loader_sha256=s.get('loader_sha256'),stock_recovered=True,accepted=good,measurement=s.get('receipt'))
    with b.Queue():
        current(b,s)
        qs.atomic(Path(s['folder'])/'result.json',json.dumps(result,indent=2)+'\n')
        # Preserve unrelated ordinary cooling. Verified recovery owns every other type of hold.
        (Path(b.DIR)/'in2010-quarantine.json').unlink()
    print('结果已保存，已核验同端口 stock FASTBOOT。', Path(s['folder'])/'result.json',flush=True)
    return 0 if good else 1


def verify_handoff(b,ticket,usb):
    rows=[json.loads(line) for line in (Path(b.DIR)/'history.jsonl').read_text().splitlines()]
    done=next((x for x in reversed(rows) if x.get('id')==ticket),{})
    need(done.get('state')=='ended' and done.get('rc')==0 and not done.get('left_running'),'stock source ticket not verified')
    if done.get('who')=='portable-handoff':
        record=qs.load(OUTPUT/f'stock-{ticket}.json')
        need(record['ticket']==ticket and record['session']==usb and done['began']<=record['epoch']<=done['ended']+1,'stale cabled stock handoff')
    elif done.get('who')=='portable':
        prior=qs.load(OUTPUT/f'ticket-{ticket}'/'session.json')
        proof=qs.load(OUTPUT/f'ticket-{ticket}'/'result.json')
        need(proof['accepted'] and proof['stock_recovered'] and prior['stock_after']==usb,'stale portable stock handoff')
    else:raise ValueError('unverified source for stock handoff')


def inside(mode,after_stock):
    b=recovery.bootq()
    tid=int(os.environ.get('BOOTQ_TICKET','0'))
    files=artifacts()
    with hardware_locks(b):
        folder=OUTPUT/f'ticket-{tid}';folder.mkdir(mode=0o700)
        token=secrets.token_hex(16)
        s=dict(id=token,mode=mode,ticket=tid,folder=str(folder),loader_sha256=LOADER_SHA,profile=json.loads((HERE/'profile.json').read_text()),cmdline=cmdline(mode,token,int(time.time())))
        with b.Queue():
            t=qs.load(Path(b.DIR)/f'ticket-{tid}.json')
            need(t.get('state')=='running' and t['pid']==os.getppid() and t.get('who')=='portable'
                 and b.quarantine() is None,'not exclusive bootq worker')
            usb=stock();verify_handoff(b,after_stock,usb);update(s,stock=usb,stock_ticket=after_stock)
            # Pin before issuing the first boot. Process death or a missing phone cannot release it.
            hold=dict(ticket=tid,runs=[],began=t['began'],ended=time.time(),normal_until=0,
                      prior=dict(ticket=tid,epoch=time.time(),session=usb),generation=secrets.token_hex(32),portable_id=token)
            qs.pin(b.DIR,hold)
        with tempfile.TemporaryDirectory(dir=folder) as temp:
            package=Path(temp)/'ram.bin'
            payload(package,[('kernel',files['kernel']),('ramdisk',files['root'])],s['cmdline'])
            need(package.stat().st_size<=0x03f00000,'RAM payload too big')
            need(stock()==usb,'stock endpoint changed before boot')
            fb(cold.STOCK,'boot',files['image'],limit=45)
            wait_for(cold.LOADER,55)
            dev=recovery.mac_usb_session(cold.LOADER)
            need(recovery.fresh(usb,dev) and cold.variable(cold.LOADER,'board')=='instantnoodle'
                 and cold.variable(cold.LOADER,'portable')=='ram-log-v1'
                 and cold.variable(cold.LOADER,'result')=='idle','portable loader not established')
            update(s,developer=dev)
            fb(cold.LOADER,'oem','ramdisk')
            fb(cold.LOADER,'stage',files['resident'],limit=90)
            fb(cold.LOADER,'stage',package,limit=90)
            need(recovery.mac_usb_session(cold.LOADER)==dev,'developer changed during stage')
            update(s,attempted=True)
            fb(cold.LOADER,'oem','run',limit=15)  # one shot, even if acknowledgement is lost
        observe(s,TIMERS[mode]+240)
        return finish(b,s)


def resume(tid,seconds):
    b=recovery.bootq()
    s=qs.load(OUTPUT/f'ticket-{tid}'/'session.json')
    need(s.get('attempted') and s['ticket']==tid,'no matching attempted boot to collect')
    with hardware_locks(b):
        with b.Queue() as q:
            current(b,s)
            need(not any(t['state']=='running' or 'recovery_for' in t for t in q.tickets()),
                 'original or recovery ticket still active')
        if not s.get('reset_pending'):observe(s,seconds)
        return finish(b,s)


def inspect_session(tid):
    b=recovery.bootq()
    s=qs.load(OUTPUT/f'ticket-{tid}'/'session.json')
    def decode(raw,result,session):
        evidence=info(raw,result,session)
        # Only return finite booleans/counts. Never copy a raw console line from the phone.
        evidence.update(modem_level3=b'modem: modem=3:' in raw,loc_start=b'generic GNSS start accepted' in raw,
                        denied403=bool(re.search(rb'(?:HTTP/1\.[01]|status(?:Code)?[=: ]+)  ?403\b',raw,re.I)),
                        denied409=bool(re.search(rb'(?:HTTP/1\.[01]|status(?:Code)?[=: ]+)  ?409\b',raw,re.I)),
                        battery_plugged=bool(re.search(rb'(?m)^battery: [^\r\n]{0,220} plugged\b',raw)),
                        battery_unplugged=bool(re.search(rb'(?m)^battery: [^\r\n]{0,220} unplugged\b',raw)),
                        cable_restored=b"battery: the cable is out" in raw,
                        phone_helper_errors=raw.count(b'portable_http['),typed_start_marker=b'portable-start-rejected-v2' in raw)
        return evidence
    with hardware_locks(b):
        with b.Queue() as q:
            current(b,s)
            need(not any(t['state']=='running' or 'recovery_for' in t for t in q.tickets()),'hardware owner active')
        item=sample(s,decode)
        need(item and (item[0]==s['developer'] or recovery.fresh(s['developer'],item[0])),'session changed')
        record=dict(ticket=tid,observed=int(time.time()),**item[1])
        qs.atomic(Path(s['folder'])/'inspection.json',json.dumps(record,indent=2)+'\n')
        print(json.dumps(record));return 0


def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group()
    g.add_argument('--run',choices=TIMERS);g.add_argument('--inside',choices=TIMERS);g.add_argument('--resume',type=int);g.add_argument('--inspect',type=int)
    p.add_argument('--seconds',type=int,default=300);p.add_argument('--after-stock',type=int);a=p.parse_args()
    if not(a.run or a.inside or a.resume or a.inspect):
        artifacts()
        for mode in TIMERS:
            line=cmdline(mode,'a'*32,1000000000)
            prefix=(ROOT.parent/'oneplus8-audit/gpu-plan/observed-loader-prefix.txt').read_bytes()
            need(len(prefix)+len(line)<=591,'cmdline lacks safe headroom')
            print(mode,len(prefix)+len(line),'bytes;',TIMERS[mode],'s')
        return 0
    need(sys.platform=='darwin' and not any(os.environ.get(k) for k in
         ('BOOTQ_DIR','BOOTQ_LOGS','BOOTQ_PHONE','BOOTQ_GETVAR','BOOTQ_STOP','ANDROID_SERIAL')),
         'macOS main physical queue required')
    if a.run or a.inside:
        need(a.after_stock and a.after_stock>0,'specify the verified stock-producing ticket using --after-stock N')
    if a.run:
        need(not os.environ.get('BOOTQ_TICKET'),'submit outside existing ticket')
        if a.run=='gnss':
            past=[qs.load(f) for f in OUTPUT.glob('ticket-*/result.json')]
            need(any(v.get('mode')=='rehearsal' and v.get('accepted') and v.get('stock_recovered') and v.get('loader_sha256')==LOADER_SHA and v.get('profile')==json.loads((HERE/'profile.json').read_text()) for v in past),
                 'physical no-modem rehearsal with a matched reconnect must pass first')
        cmd='exec '+shlex.join([sys.executable,str(Path(__file__).resolve()),'--inside',a.run,'--after-stock',str(a.after_stock)])
        return subprocess.run([ROOT/'tools/bootq','run','--who','portable','--what',f'On-phone portable {a.run}',
                               '--no-boot-ok','--max',str(TIMERS[a.run]+550),'--',cmd],cwd=ROOT).returncode
    if a.inspect:return inspect_session(a.inspect)
    if a.resume:
        need(not os.environ.get('BOOTQ_TICKET') and 5<=a.seconds<=600,'invalid collection request')
        return resume(a.resume,a.seconds)
    return inside(a.inside,a.after_stock)


if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,KeyError,OSError,subprocess.SubprocessError,cold.RecoveryError) as exc:
        print('portable:',str(exc),file=sys.stderr);sys.exit(1)
