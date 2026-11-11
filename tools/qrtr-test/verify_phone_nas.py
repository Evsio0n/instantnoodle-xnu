"""Verify the atomic two-CID NAS read without treating an LTE attach as a requirement."""
import argparse,json,re
from pathlib import Path

P='modem: ui_qmux: '
ALLOC=re.compile(r'allocate epoch=(\d+) cid1=(\d+) local1=(\d+) cid2=(\d+) local2=(\d+)$')
SEND=re.compile(r'submit epoch=(\d+) cid=(\d+) local=(\d+) node=(\d+) peer=(\d+) tid=(\d+) msg=(\d+) rc=(-?\d+)$')
REPLY=re.compile(r'reply epoch=(\d+) cid=(\d+) local=(\d+) node=(\d+) peer=(\d+) tid=(\d+) msg=(\d+) bytes=(\d+) result=(-?\d+) error=(-?\d+) reg=(-?\d+) cs=(-?\d+) ps=(-?\d+)$')
END=re.compile(r'end epoch=(\d+) reason=(\w+) attempts=(\d+) replies=(\d+) valid=(\d+) released=([01])$')

def verify(lines):
    lines={l[len(P):] for l in lines if l.startswith(P)}
    allocations={tuple(map(int,m.groups())) for l in lines if (m:=ALLOC.fullmatch(l))}
    sends={tuple(map(int,m.groups())) for l in lines if (m:=SEND.fullmatch(l))}
    replies={tuple(map(int,m.groups())) for l in lines if (m:=REPLY.fullmatch(l))}
    endings={m.groups() for l in lines if (m:=END.fullmatch(l))}
    result={'checks_pass':False,'two_client_routing_verified':False,'serving_data_verified':False,'serving_values':[],'allocations':sorted(allocations),'sends':sorted(sends),'replies':sorted(replies),'end':sorted(endings)}
    if len(allocations)!=1 or len(sends)!=2 or len(replies)!=2 or len(endings)!=1:return result
    epoch,c1,p1,c2,p2=next(iter(allocations))
    if c1==c2 or p1==p2 or not (0<c1<255 and 0<c2<255 and p1>=0x4000 and p2>=0x4000):return result
    pair={(c1,p1),(c2,p2)}
    if {(x[1],x[2])for x in sends}!=pair or {(x[1],x[2])for x in replies}!=pair:return result
    endpoints={(x[3],x[4])for x in sends}
    if len(endpoints)!=1 or not all(next(iter(endpoints))):return result
    if any(x[0]!=epoch or x[5:8]!=(30115,36,0) for x in sends):return result
    valid=0
    for r in sorted(replies):
        if (r[0]!=epoch or (r[3],r[4]) not in endpoints or r[5:7]!=(30115,36) or not 14<=r[7]<=8192):return result
        if (r[8],r[9])==(0,0):
            if r[7]<22 or not all(0<=n<=255 for n in r[10:13]):return result
            valid |= 1 if r[1]==c1 else 2
            result['serving_values'].append(dict(cid=r[1],registration=r[10],cs=r[11],ps=r[12]))
        elif r[8]!=1 or not 0<r[9]<=65535:
            return result
        # On a firmware error, raw optional TLVs in the retained record are
        # unqualified and must not become an attachment or registration state.
    if next(iter(endings))!=(str(epoch),'both_replies','2','3',str(valid),'1'):return result
    result['two_client_routing_verified']=True
    result['serving_data_verified']=valid==3
    result['checks_pass']=valid==3
    return result

def evidence(paths,owner=False):
    lines=set();events=[];seen=set();options=0
    required={'ui_qmux=2' if owner else 'ui_qmux=1','wifi=0','modem=3','modem_mission=1','modem_radio=0' if owner else 'modem_radio=6','modem_loc=0','modem_gnss=0','ufs_write=0','var_store=0'}
    for p in sorted(paths,key=lambda f:int(f.name.rsplit(".",1)[-1])):
        raw=p.read_bytes();prefix,separator,body=raw.rpartition(b'== KERNEL LOG (the kernel is still running) ==')
        if not separator:continue
        cmd=re.findall(rb'^cmdline: ([^\r\n]+)',prefix,re.M)
        if not cmd:continue
        words=cmd[-1].decode(errors='replace').split()
        if not all([v for v in words if v.partition('=')[0]==t.partition('=')[0]]==[t] for t in required):continue
        options+=1
        lines.update(l.decode('ascii','replace').strip() for l in body.splitlines() if l.startswith(P.encode()))
        if owner:
            for raw_line in body.splitlines():
                if raw_line.startswith((P.encode(),b'modem: qmux-owner: ',b'modem: a second after the last reset: ',b'modem: level 3 is done')):
                    line=raw_line.decode('ascii','replace').strip()
                    if line not in seen:
                        seen.add(line);events.append(line)
    report=dict(verify(lines),matching_windows=options)
    if owner:
        report['owner_lifecycle']=events
        def hits(pattern):
            return [(index,m) for index,line in enumerate(events) if(m:=re.fullmatch(pattern,line))]
        starts=hits(r'modem: qmux-owner: poller attached after mission epoch=(\d+)')
        data=hits(r'modem: ui_qmux: .+')
        ing=hits(r'modem: qmux-owner: ingress closed accepted=(\d+) retired=(\d+) discarded=(\d+) overflow=(\d+) rejected=(\d+)')
        off=hits(r'modem: qmux-owner: callbacks exited, channels detached, link down')
        ack=hits(r'modem: qmux-owner: mission observed drain ACK before release')
        pins=hits(r"modem: a second after the last reset: MDM2AP status 0, errfatal 0; AP2MDM status 0; PERST 0.*")
        done=hits(r'modem: level 3 is done')
        qualified=all(len(rows)==1 for rows in (starts,ing,off,ack,pins,done)) and bool(data)
        if qualified:
            n,retired,discarded,over,bad=map(int,ing[0][1].groups())
            qualified=(n>0 and n==retired and discarded==over==bad==0 and
                       starts[0][0]<data[0][0]<=data[-1][0]<ing[0][0]<off[0][0]<ack[0][0]<pins[0][0]<done[0][0] and
                       len(report['allocations'])==1 and int(starts[0][1][1])==report['allocations'][0][0])
        report['owner_drain_verified']=qualified
        report['checks_pass'] &= qualified
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run');parser.add_argument('--owner',action='store_true');a=parser.parse_args()
    if not re.fullmatch(r'\d{6}',a.run):parser.error('expected one six-digit fresh phone run')
    root=Path(__file__).resolve().parents[2]
    report=evidence((root/'logs').glob(a.run+'-run.log.live.*'),owner=a.owner)
    print(json.dumps(report,indent=2));raise SystemExit(0 if report['checks_pass'] else 1)
