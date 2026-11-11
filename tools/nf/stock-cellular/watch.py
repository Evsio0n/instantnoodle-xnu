"""Inside an owned handset ticket, locate the stock cellular row by actual framebuffer text."""
import argparse,json,os,re,subprocess,time
from pathlib import Path

K=Path(__file__).resolve().parents[3]
OUT=K.parent/'oneplus8-audit/stock-cellular-ui'
HOME={'facetime','calendar','photos','maps','clock','wallet','health'}
WIRE={'airplane','wifi','bt','vpn','hotspot'}
NEXT={'notifications','sounds','screen_time','focus'}
METHODS={'agent','tap','swipe'}

def read_frame(path):
    obj=json.loads(subprocess.check_output([str(OUT/'screen'),str(path)],timeout=9))
    return [(v['id'],(v['box'][0]+v['box'][2]/2)/obj['width'],(v['box'][1]+v['box'][3]/2)/obj['height']) for v in obj['labels']]

def classify(rows):
    ids={r[0]for r in rows}
    if 'modal' in ids:return 'dialog'
    if any(i=='settings' and y>.15 for i,x,y in rows) and len(HOME&ids)>=4:return 'home'
    if any(i=='cellular' and .25<x<.78 and y<.13 for i,x,y in rows) and len(ids&{'settings','network','sims','carrier','cell_pane'})>0:return 'pane'
    if any(i=='settings' and y<.19 for i,x,y in rows) and len(ids&(WIRE|NEXT|{'general','display','battery','root_privacy','passcode'}))>=2:return 'settings'
    return 'unknown'

def direct(method,*args):
    assert method in METHODS
    try:
        p=subprocess.run([str(K/'iphont'),method,*args],cwd=K,capture_output=True,timeout=22)
        return json.loads(p.stdout)
    except (OSError,ValueError,subprocess.TimeoutExpired):
        return {'ok':False,'error':{'code':'no_verifiable_reply'}}

def argument(n):return str(round(n*65535))

def watch(ticket,folder,deadline,not_before):
    folder.mkdir(parents=True,exist_ok=True);journal=folder/'native-cellular-screen.json'
    info={'ticket':ticket,'actions':[],'frames':[],'native_row_visible':None,'native_pane_visible':None,'coverage':'not observed'}
    def save():
        temp=journal.with_suffix('.tmp');temp.write_text(json.dumps(info,indent=2)+'\n');temp.replace(journal)
    save();boot=None;agent=False;opened=False;cell_tap=False;scroll=0;prev=None;rows_prior=None
    while time.monotonic()<deadline:
        time.sleep(2)
        if boot is None:
            for path in sorted((K/'logs').glob('??????-run.log.live.*'),key=lambda f:f.stat().st_mtime,reverse=True)[:8]:
                if path.stat().st_mtime<not_before:continue
                body=path.read_bytes()
                if any(all(k in l.split() for k in (b'wifi=0',b'modem_loc=0',b'modem_gnss=0',b'iphont_control=1',b'root_apfs=ufs_darwin'))
                    for l in body.splitlines() if l.startswith(b'cmdline:')) and b'stock_cellular_ui' in body:
                    boot=path.name[:6];info['run']=boot;save();break
            if boot is None:continue
        if (K/'logs'/f'{boot}-run.log').exists():break
        f=folder/f'stock-{boot}-{time.monotonic_ns()}.png'
        p=subprocess.run(['bash','-ec','. common/lib.sh; load_device instantnoodle; ./xl screenshot "$1"','bash',str(f)],cwd=K,
            capture_output=True,timeout=24)
        if p.returncode or not f.is_file():continue
        try:rows=read_frame(f)
        except (ValueError,OSError,subprocess.SubprocessError):continue
        pg=classify(rows);ids={i for i,x,y in rows};same=(pg==prev)
        info['frames'].append({'file':f.name,'page':pg,'labels':rows});save();prev=pg
        if pg=='dialog':info['coverage']='stopped at system dialog';save();return
        if pg=='unknown' or not same:rows_prior=rows;continue
        if pg=='pane':
            info.update(native_row_visible=True,native_pane_visible=True,coverage='stock cellular pane observed',pane_frame=f.name);save();return
        if not agent:
            ack=direct('agent');d=ack.get('result',{}).get('data',{})
            if not ack.get('ok') or not d.get('control') or d.get('userspace'):
                info.update(coverage='USB input unavailable',error=ack.get('error',{}).get('code','unexpected_handshake'));save();return
            agent=True
        cmd=None
        if pg=='home' and not opened:
            found=[(x,y) for i,x,y in rows if i=='settings' and .16<y<.92]
            if len(found)==1:
                x,y=found[0];cmd=('tap',argument(x),argument(y-.05));opened=True
        elif pg=='settings':
            found=[(x,y) for i,x,y in rows if i=='cellular' and .13<y<.93 and .08<x<.59]
            if len(found)==1 and not cell_tap:
                x,y=found[0];info.update(native_row_visible=True,row_frame=f.name);cmd=('tap',argument(x),argument(y));cell_tap=True
            elif len(found)==0 and cell_tap:info['coverage']='row present; original pane transition pending'
            elif len(found)==0:
                # Row between Bluetooth and the following settings family. Two
                # steady frames bracket the entire search region. A negative
                # classification still requires final visual inspection.
                anchors=sorted((y,i) for i,x,y in rows if i in WIRE|NEXT)
                bt=[y for y,i in anchors if i=='bt'];later=[y for y,i in anchors if i in NEXT|{'vpn','hotspot'}]
                if bt and any(y>bt[0]+.025 for y in later) and rows_prior is not None and 'bt' in {r[0] for r in rows_prior}:
                    info.update(coverage='wireless region bracketed; review screenshot for missing row',wireless_frame=f.name);save();return
                if scroll<3:
                    # Scroll only the list center. No right-hand switch receives a tap.
                    cmd=('swipe','32000','20000','32000','52000','--duration','550','--steps','10');scroll+=1
        rows_prior=rows
        if cmd:
            resp=direct(*cmd);info['actions'].append({'command':cmd[0],'target':pg,'frame':f.name,'ack':resp.get('ok') is True})
            if not resp.get('ok'):
                info.update(coverage='unacknowledged input; no replay',error=resp.get('error',{}).get('code','unknown'));save();return
            prev=None;rows_prior=None;save();time.sleep(1.3)
    info['coverage']=info['coverage'] if info['native_row_visible'] or info['coverage']!='not observed' else 'ended before wireless group established';save()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--frames',required=True,type=Path);p.add_argument('--not-before',required=True,type=float);p.add_argument('--seconds',type=int,default=245);a=p.parse_args()
    ticket=os.environ.get('BOOTQ_TICKET','')
    if not ticket.isdecimal():raise SystemExit('requires the active cellular operator ticket')
    os.umask(0o077);watch(int(ticket),a.frames,time.monotonic()+a.seconds,a.not_before)
