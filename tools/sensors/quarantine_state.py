"""Disk-only IN2010 hold discovery. Call state mutations with bootq.Queue locked."""
import hashlib,json,os,re,sys,tempfile,time
from pathlib import Path

WEEK=7*86400
def atomic(path,data):
    path=Path(path)
    with tempfile.NamedTemporaryFile('w',dir=path.parent,delete=False) as f:
        tmp=Path(f.name);f.write(data);f.flush();os.fsync(f.fileno())
    os.replace(tmp,path)
    d=os.open(path.parent,os.O_RDONLY)
    try:os.fsync(d)
    finally:os.close(d)
def load(path):
    return json.loads(Path(path).read_text())
def session(value):
    return isinstance(value,dict) and all(type(value.get(k)) is int and value[k]>0 for k in ('locationID','IORegistryEntryID','sessionID'))
def held_receipt(root,queue,began,ticket):
    candidates=[]
    for p in [*(Path(root)/'out/eye').glob('sensor-ticket-*/end-cold.json'),*Path(queue).glob('recovery-*/receipt.json')]:
        try:
            v=load(p);rid=int(v['ticket']);stamp=float(v['epoch']);s=v['session']
            if (rid>=ticket or stamp>began or not session(s) or
                (p.name=='end-cold.json' and p.parent.name!=f'sensor-ticket-{rid}')):continue
            candidates.append((stamp,rid,p,s))
        except (OSError,TypeError,ValueError,KeyError):continue
    if not candidates:return None
    stamp,rid,p,s=max(candidates,key=lambda x:(x[0],x[1]))
    # Match the receipt to the actual producer ticket and its time window.
    for line in reversed((Path(queue)/'history.jsonl').read_text().splitlines()):
        v=json.loads(line)
        if v.get('id')==rid:
            expected='sensor' if p.name=='end-cold.json' else 'recovery'
            if v.get('who')==expected and v.get('state')=='ended' and v.get('rc')==0 and v.get('began',float('inf'))<=stamp<=v.get('ended',0)+2:
                return dict(ticket=rid,epoch=stamp,session=s)
            break
    return None
def make_hold(root,queue,logs,ticket,normal_until=0):
    if sys.platform!='darwin' or not ticket.get('runs'):return None
    try:
        source=held_receipt(root,queue,ticket['began'],ticket['id'])
    except (OSError,ValueError,KeyError):return None
    if not source:return None
    hit=[]
    for run in ticket['runs']:
        if not re.fullmatch(r'\d{6}',run):continue
        board=False;cold=False
        for suffix in ('-run.log','-run.log.live'):
            f=Path(logs)/(run+suffix)
            try:
                if not ticket['began']-1<=f.stat().st_mtime<=ticket['ended']+3:continue
                with f.open('rb') as src:
                    raw=src.read(2<<20)
                board|=bool(re.search(rb'(?m)^cmdline: [^\n]*\bboard=instantnoodle(?:\s|$)',raw))
                cold|=b'cold recovery required' in raw
            except OSError:pass
        if board and cold:hit.append(run)
    if not hit:return None
    identity=dict(ticket=ticket['id'],runs=hit,began=ticket['began'],ended=ticket['ended'],prior=source)
    gen=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
    return dict(**identity,generation=gen,normal_until=normal_until)
def state(root,queue,logs):
    directory=Path(queue);path=directory/'in2010-quarantine.json'
    if path.exists():
        return load(path) # a corrupt durable record fails closed
    if sys.platform!='darwin':return None
    try:
        deadline=float((directory/'cool_until').read_text())
        if deadline<=0:return None
        for line in reversed((directory/'history.jsonl').read_text().splitlines()):
            t=json.loads(line)
            if t.get('state')!='ended' or not t.get('runs'):continue
            if abs(deadline-(t['ended']+WEEK))>15:return None
            return make_hold(root,queue,logs,t,ordinary_deadline(directory,t))
    except (OSError,ValueError,KeyError):return None
    return None
def ordinary_deadline(directory,t):
    if not (Path(directory)/'cool').exists():return 0
    return t['ended']+min(300,(t['ended']-t['began'])*float(os.environ.get('BOOTQ_COOL_SHARE') or .5))
def pin(directory,h):
    atomic(Path(directory)/'in2010-quarantine.json',json.dumps(h,sort_keys=True)+'\n')
