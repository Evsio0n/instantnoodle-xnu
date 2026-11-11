"""Fixed IN2010 queue operation. A receipt is committed while the hardware and queue locks are held."""
import argparse,contextlib,fcntl,hashlib,importlib.machinery,importlib.util,json,os,plistlib,re,subprocess,sys,time
from pathlib import Path
import cold_recover as cold
import quarantine_state as qs

ROOT=cold.ROOT
DEVELOPER_SHA='5fea3ea2683e6555b90602cc0a19272705d24fb2af9d12f009ed8edca20853b4'
def guard(ok,why):
    if not ok:raise cold.RecoveryError(why)
def bootq():
    loader=importlib.machinery.SourceFileLoader('in2010_bootq',str(ROOT/'tools/bootq'))
    spec=importlib.util.spec_from_loader(loader.name,loader);m=importlib.util.module_from_spec(spec);loader.exec_module(m);return m
def mac_usb_session(serial):
    raw=subprocess.check_output(['ioreg','-a','-p','IOUSB','-l'],timeout=5)
    def walk(obj):
        if isinstance(obj,list):
            for node in obj:yield from walk(node)
        elif isinstance(obj,dict):
            if obj.get('USB Serial Number')==serial:
                yield {k:obj.get(k) for k in ('sessionID','IORegistryEntryID','locationID')}
            yield from walk(obj.get('IORegistryEntryChildren',[]))
    match=list(walk(plistlib.loads(raw)))
    guard(len(match)==1 and qs.session(match[0]),'one complete matching macOS USB session is required')
    return match[0]
def fresh(a,b):
    return (a['locationID']==b['locationID'] and a['sessionID']!=b['sessionID'] and
            a['IORegistryEntryID']!=b['IORegistryEntryID'])
def wait_endpoint(seconds,notify,authorize):
    deadline=time.monotonic()+seconds;last_note=-100.
    while time.monotonic()<deadline:
        authorize()
        try:seen=cold.devices()
        except (OSError,subprocess.SubprocessError,cold.RecoveryError):seen=set()
        if seen:
            guard(len(seen)==1 and seen<={cold.STOCK,cold.LOADER},'unexpected or ambiguous fastboot endpoints')
            return next(iter(seen))
        if time.monotonic()-last_note>=30:
            notify('pending-no-endpoint');last_note=time.monotonic()
        time.sleep(min(2,max(0,deadline-time.monotonic())))
    raise cold.RecoveryError('pending: no confirmed target before the observation deadline')
def prove(prior,seconds,notify,authorize,usb=mac_usb_session):
    seen=wait_endpoint(seconds,notify,authorize)
    stages=[]
    def measure(phase,serial):
        authorize()
        guard(cold.devices()=={serial},'fastboot endpoint changed or is ambiguous')
        s=usb(serial)
        guard(s['locationID']==prior['locationID'],'device is on another USB port')
        if serial==cold.STOCK:
            guard(cold.variable(serial,'product')=='kona','stock hardware product mismatch')
        else:
            guard(cold.variable(serial,'product')=='xnuloader','unexpected developer loader product')
            result=cold.variable(serial,'result')
            if phase=='idle':guard(result=='idle','developer loader is not idle')
            else:guard(result=='idle' or re.fullmatch(r'(?:returned|hung) [0-9]+ [0-9]+',result),'loader is busy or has an unknown state')
        return s
    if seen==cold.STOCK:
        # Stock on first sight could predate the fault. Establish a developer source first.
        prep={}
        def preparing(phase,serial):
            s=measure(phase,serial)
            if phase=='source':guard(serial==cold.STOCK,'initial stock identity changed')
            if phase=='stock':prep['stock']=s
            if phase=='developer-boot':guard(s==prep.get('stock'),'stock changed before the RAM boot')
            if phase=='idle':guard(fresh(prep['stock'],s),'developer preparation has no fresh session')
            notify('prepare-'+phase,session=s)
        cold.recover(True,False,120,witness=preparing)
    def witness(phase,serial):
        s=measure(phase,serial)
        expected=('source','reset-request','stock','developer-boot','idle')
        guard(len(stages)<len(expected) and expected[len(stages)]==phase,'did not witness a complete developer-to-stock reset')
        if phase=='source':guard(serial==cold.LOADER,'a developer reset source is required')
        elif phase=='reset-request':guard(s==stages[0]['session'],'source changed before reset')
        elif phase=='stock':guard(fresh(stages[0]['session'],s) and fresh(prior,s),'stock session did not change after reset')
        elif phase=='developer-boot':guard(s==stages[2]['session'],'stock changed before developer RAM boot')
        else:
            guard(all(fresh(x,s) for x in (prior,stages[0]['session'],stages[2]['session'])),'final developer session is stale')
        stages.append(dict(stage=phase,serial=serial,session=s,epoch=time.time()));notify(phase,session=s)
    cold.recover(True,False,120,witness=witness)
    guard(len(stages)==5,'incomplete observed reset chain')
    guard(measure('idle',cold.LOADER)==stages[-1]['session'],'USB identity changed before proof was committed')
    return stages
def ownership(b,ticket,gen=None):
    queue=Path(b.DIR)
    current=qs.load(queue/f'ticket-{ticket}.json');h=b.quarantine()
    guard(current.get('id')==ticket and current.get('state')=='running' and current.get('pid')==os.getppid(),'operation does not own its active bootq ticket')
    guard(h and current.get('recovery_for')==h['ticket'] and current.get('recovery_generation')==h['generation'] and
          (gen is None or h['generation']==gen),'quarantine generation changed')
    return h
def commit(b,ticket,h,stages,folder):
    with b.Queue():
        ownership(b,ticket,h['generation'])
        record=dict(ticket=ticket,epoch=time.time(),source_ticket=h['ticket'],source_runs=h['runs'],generation=h['generation'],
                    previous=h['prior'],stages=stages,session=stages[-1]['session'],developer_image_sha256=DEVELOPER_SHA)
        qs.atomic(folder/'receipt.json',json.dumps(record,indent=2)+'\n')
        # Legacy hosts see a normal timer again. Keep the proof-bearing marker until this is durable.
        directory=Path(b.DIR)
        remaining=h['normal_until'] if (directory/'cool').exists() else 0
        qs.atomic(directory/'cool_until',str(remaining))
        (directory/'in2010-quarantine.json').unlink()
        fd=os.open(directory,os.O_RDONLY)
        try:os.fsync(fd)
        finally:os.close(fd)
def run(seconds):
    guard(sys.platform=='darwin','macOS session evidence is required for this operation')
    guard(not any(os.environ.get(k) for k in ('BOOTQ_DIR','BOOTQ_PHONE','BOOTQ_LOGS','BOOTQ_GETVAR','BOOTQ_STOP')),'a test/private queue cannot operate real hardware')
    ticket=int(os.environ.get('BOOTQ_TICKET','0'));guard(ticket>0,'fixed bootq recovery ticket required')
    file=ROOT/'out/instantnoodle-fwreader.img'
    with file.open('rb') as f:guard(hashlib.file_digest(f,'sha256').hexdigest()==DEVELOPER_SHA,'developer image changed')
    b=bootq();directory=Path(b.DIR)
    with b.Queue():hold=ownership(b,ticket)
    folder=directory/f'recovery-{ticket}';folder.mkdir(mode=0o700,exist_ok=True)
    def authorize():
        with b.Queue():ownership(b,ticket,hold['generation'])
    def note(event,**info):
        v=dict(epoch=time.time(),event=event,ticket=ticket,source=hold['ticket'],generation=hold['generation'],**info)
        with (folder/'events.jsonl').open('a') as f:f.write(json.dumps(v)+'\n');f.flush();os.fsync(f.fileno())
        qs.atomic(folder/'pending.json',json.dumps(v)+'\n');print('recovery:',event,flush=True)
    note('pending',prior=hold['prior'])
    try:
        with contextlib.ExitStack() as locks:
            for suffix in ('.run','.lock'):
                f=locks.enter_context(open(b.PHONE+suffix,'a'))
                fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
            stages=prove(hold['prior']['session'],seconds,note,authorize)
            commit(b,ticket,hold,stages,folder)
        note('verified');print(folder/'receipt.json',flush=True)
    except (cold.RecoveryError,OSError,ValueError,subprocess.SubprocessError) as exc:
        note('pending-attention',reason=str(exc));return 1
    return 0
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--seconds',required=True,type=int);a=p.parse_args()
    try:
        guard(1<=a.seconds<=86400,'invalid polling deadline');sys.exit(run(a.seconds))
    except (cold.RecoveryError,OSError,ValueError,subprocess.SubprocessError) as exc:
        print('recovery:',exc,file=sys.stderr);sys.exit(1)
