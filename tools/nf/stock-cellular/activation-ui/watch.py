"""Visual-only evidence from one cellular operator ticket. No modem/permission actions."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

HERE = Path(__file__).resolve().parent
K = HERE.parents[3]
SCREEN = K.parent/'oneplus8-audit/stock-cellular-ui/activation-ui/screen'
HOME = {'facetime','calendar','photos','maps','clock','wallet','health','weather','stocks','find_my',
        'home_app','books','store','watch_app','shortcuts','files','tips','freeform','utilities',
        'fitness','translate','contacts_app','reminders_app','mail','notes','news','app_store','podcasts'}
WIRE = {'airplane','wifi','bt','vpn','hotspot'}
NEXT = {'notifications','sounds','screen_time','focus'}
SECTIONS = {'sim_section','gw_section','activation_section','radio_section'}
TERMINAL = {'verified','already_bound','refused'}
PHASES = {'live','stopped','waiting'}
DECISIONS = TERMINAL|{'not_submitted','submitted','accepted','uncertain'}
DATA = {'app_data','slot_data','gw_data','dms_data','nas_data','wds_data','cs_attach','ps_attach','usim_state'}
REQUIRED = {'wifi':'0','modem_loc':'0','modem_gnss':'0','modem':'3','ufs_write':'0','var_store':'0',
            'iphont_control':'1','root_apfs':'ufs_darwin','home_apps':'1'}

@dataclass(frozen=True)
class Mark:
    kind: str
    x: float
    y: float
    value: str

def recognize(path):
    result = subprocess.run([str(SCREEN),str(path)],capture_output=True,timeout=12,check=True)
    data = json.loads(result.stdout)
    marks = []
    for item in data['labels']:
        x,y,w,h = item['box']
        marks.append(Mark(item['id'],(x+w/2)/data['width'],(y+h/2)/data['height'],item['label']))
    return marks

def kinds(rows):
    return {m.kind for m in rows}

def classify(rows):
    ids = kinds(rows)
    if 'modal' in ids:return 'dialog'
    if len(ids & SECTIONS)>=2 and ('monitor' in ids or 'open_settings' in ids or ids & PHASES):
        return 'monitor'
    if len(ids & HOME)>=4 and any(m.y>.15 and m.kind in HOME for m in rows):
        return 'home'
    if any(m.kind=='cellular' and .23<m.x<.78 and m.y<.14 for m in rows) and ids & {'settings','network','sims','carrier','cell_pane','data_usage'}:
        return 'cellular_pane'
    if any(m.kind=='settings' and m.y<.20 for m in rows) and len(ids&(WIRE|NEXT|{'general','battery','display','sign_in','root_privacy'}))>=2:
        return 'settings'
    if any(m.kind=='settings' and m.x<.21 and m.y<.12 for m in rows) and any(m.kind=='monitor' and .22<m.x<.78 and m.y<.16 for m in rows):
        return 'app_settings'
    return 'unknown'

def target(rows,kind,xrange,yrange):
    found = [m for m in rows if m.kind==kind and xrange[0]<m.x<xrange[1] and yrange[0]<m.y<yrange[1]]
    return found[0] if len(found)==1 else None

def stable_target(now,then,kind,xrange,yrange):
    a,b = target(now,kind,xrange,yrange),target(then,kind,xrange,yrange)
    if a and b and abs(a.x-b.x)<.025 and abs(a.y-b.y)<.025:return a
    return None

def wireless_bounds(rows):
    seen = {}
    for m in rows:
        if m.kind in WIRE|NEXT and .045<m.x<.68 and .12<m.y<.95:
            seen.setdefault(m.kind,[]).append(m.y)
    if not all(len(seen.get(k,[]))==1 for k in ('airplane','wifi','bt')):
        return None
    after = [v for k in NEXT for v in seen.get(k,[]) if v>seen['bt'][0]+.025]
    if not after or not seen['airplane'][0]<seen['wifi'][0]<seen['bt'][0]<min(after):
        return None
    return seen['bt'][0], min(after)

def bracketed(rows):
    return wireless_bounds(rows) is not None

def axis(n):return str(round(n*65535))
def tap(x,y):return ('tap',axis(x),axis(y),'--duration','0.085')
def swipe(x1,y1,x2,y2):
    return ('swipe',axis(x1),axis(y1),axis(x2),axis(y2),'--duration','0.53','--steps','12')

def command(args):
    if args[0] not in {'agent','tap','swipe','button'} or (args[0]=='button' and args!=('button','home')):
        raise ValueError('unsupported navigation command')
    try:
        response = subprocess.run([str(K/'iphont'),*args],cwd=K,capture_output=True,timeout=23)
        return json.loads(response.stdout)
    except (OSError,ValueError,subprocess.TimeoutExpired):
        return {'ok':False,'error':{'code':'unconfirmed_delivery'}}

def valid_epoch(raw,not_before):
    for line in raw.splitlines():
        if not line.startswith(b'cmdline:'):continue
        tokens=line.decode('ascii','replace').split()[1:]
        if not all([v for v in tokens if v.split('=',1)[0]==k]==[k+'='+val] for k,val in REQUIRED.items()):continue
        epochs=[v[6:] for v in tokens if v.startswith('epoch=')]
        if len(epochs)==1 and epochs[0].isdigit() and int(epochs[0])>=not_before-5:return int(epochs[0])
    return None

def current_run(not_before,requested):
    files = (K/'logs').glob((requested or '??????')+'-run.log.live')
    for path in sorted(files,reverse=True):
        try:
            if path.stat().st_mtime<not_before:continue
            raw=path.read_bytes()
        except FileNotFoundError:continue
        epoch=valid_epoch(raw,not_before)
        if epoch is not None:return path.name[:6],epoch
    return None

def evidence_mark(rows):
    ids=kinds(rows)
    return {'fields':sorted(SECTIONS&ids),'phase':sorted(PHASES&ids),'decisions':sorted(DECISIONS&ids),
            'samples':[m.value for m in rows if m.kind=='sample'],'projected_values':{m.kind:m.value for m in rows if m.kind in DATA}}

def write(path,data):
    temporary=path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data,indent=2)+'\n')
    temporary.replace(path)

class Route:
    def __init__(self,ticket,observe_online=False):
        self.observe_online=observe_online
        self.online_stable_since=None
        self.online_fallback_not_before=0
        self.info={'ticket':ticket,'actions':[],'captured_app_frames':[],'frames':0,'coverage':'waiting for current Home',
                   'project_app_visible':False,'live_sample_observed':False,'wireless_group_bracketed':False,
                   'native_cellular_row_visible':None,'native_cellular_pane_visible':None}
        self.last_page=None
        self.last_rows=[]
        self.opened_at=None
        self.first_live=None
        self.terminal_at=None
        self.best_score=-1
        self.did_open=False
        self.settings_at=None
        self.home_fallback=False
        self.page_swipes=0
        self.settings_scrolls=0
        self.settings_back=False
        self.cellular_tap=False
        self.finished=False

    def done(self,coverage):
        self.info['coverage']=coverage
        self.finished=True
        return None

    def decide(self,now,rows,frame,sha):
        pg=classify(rows);prior=self.last_rows;stable=self.last_page==pg
        self.last_page=pg;self.last_rows=rows
        self.info['frames']+=1
        self.info.update(last_page=pg,last_frame=frame)
        if pg=='dialog':return self.done('stopped at system dialog')
        if pg=='unknown':return None
        if pg=='monitor':
            if self.opened_at is None:self.opened_at=now
            mark=evidence_mark(rows)
            self.info['project_app_visible']=True
            self.info['captured_app_frames'].append({'file':frame,'sha256':sha,**mark})
            score=(5*len(mark['projected_values'])+len(mark['fields'])
                   +15*bool(set(mark['decisions']) & TERMINAL)+8*bool(mark['samples'])+8*('live' in mark['phase']))
            if self.observe_online:
                values=mark['projected_values']
                live='live' in mark['phase'] and bool(mark['samples'])
                online=live and values.get('dms_data')=='dms online'
                reg=live and values.get('nas_data')=='nas registered'
                if online:
                    self.info.update(online_frame=frame,online_frame_sha256=sha)
                    score+=100
                if reg:
                    self.info.update(registered_frame=frame,registered_frame_sha256=sha)
                    score+=40
                complete=bool(online and reg and 'wds_data' in values and 'cs_attach' in values and 'ps_attach' in values)
                if complete:
                    self.info.update(online_nas_wds_frame=frame,online_nas_wds_frame_sha256=sha)
                    score+=80
                both_attached=(complete and values.get('cs_attach')=='cs attached' and values.get('ps_attach')=='ps attached')
                if both_attached:
                    self.info.update(online_both_attached_frame=frame,online_both_attached_frame_sha256=sha)
                    if self.online_stable_since is None:self.online_stable_since=now
                    score+=40
                else:
                    self.online_stable_since=None
            if score>=self.best_score:
                self.best_score=score
                self.info.update(app_best_frame=frame,app_best_frame_sha256=sha)
            if 'live' in mark['phase'] and mark['samples']:
                self.info['live_sample_observed']=True
                if self.first_live is None:self.first_live=now
            if self.info['live_sample_observed'] and set(mark['decisions'])&TERMINAL:
                if self.terminal_at is None:self.terminal_at=now
            if not stable:return None
            if self.settings_at is not None:
                if not self.home_fallback and now-self.settings_at>=10:
                    self.home_fallback=True
                    return ('button','home'),'return Home after settings request remained in project App'
                return None
            # A decision may precede the later radio and independent card-status reads.
            # Keep collecting real frames through further updates, without trapping navigation.
            key_values={'app_data','gw_data','nas_data','wds_data'}
            settle_seconds=6 if key_values.issubset(mark['projected_values']) else 12
            dwell_ok=((self.terminal_at is not None and now-self.terminal_at>=settle_seconds) or
                      (self.first_live is not None and now-self.first_live>=35) or
                      (self.first_live is None and now-self.opened_at>=45))
            if self.observe_online:
                # Last run's stored UIM outcome has no authority over this
                # round's RF outcome. Wait on the current on-screen radio result.
                # Reserve time for the separate Settings screen even on refusal.
                dwell_ok=bool((self.online_stable_since is not None and now-self.online_stable_since>=5) or
                        (self.first_live is not None and now-self.first_live>=65 and now>=self.online_fallback_not_before) or
                        ('stopped' in mark['phase'] and now-self.opened_at>=6) or
                        (self.first_live is None and now-self.opened_at>=45))
            if not dwell_ok:return None
            button=stable_target(rows,prior,'open_settings',(.08,.92),(.13,.92))
            if button:
                self.settings_at=now
                self.info['settings_link_from']=frame
                return tap(button.x,button.y),'project App Open Settings'
            if len(set(mark['fields']))==4 and now-self.opened_at>=55 and not self.home_fallback:
                self.home_fallback=True
                self.settings_at=now
                return ('button','home'),'return Home after project App documented without a visible Settings control'
            return None
        if not stable:return None
        if pg=='home':
            if self.settings_at is not None:
                settings=stable_target(rows,prior,'settings',(.055,.95),(.14,.92))
                if settings:
                    if self.info.get('settings_home_tapped'):return None
                    self.info['settings_home_tapped']=True
                    return tap(settings.x,settings.y-.05),'stock Settings Home icon'
                if not self.info.get('returned_first_page'):
                    self.info['returned_first_page']=True
                    return swipe(.16,.53,.84,.53),'return to first Home page'
                return None
            icon=stable_target(rows,prior,'monitor',(.055,.95),(.14,.93))
            if icon:
                if self.did_open:return None
                self.did_open=True
                self.info['project_icon_frame']=frame
                return tap(icon.x,icon.y-.05),'Cellular Monitor Home icon'
            if self.did_open or target(rows,'monitor',(.055,.95),(.14,.93)):
                return None
            if self.page_swipes<2:
                self.page_swipes+=1
                return swipe(.85,.53,.15,.53),'page through confirmed SpringBoard icons'
            return self.done('project icon not identified on bounded Home pages')
        if self.settings_at is None:return None
        if pg=='app_settings' and not self.settings_back:
            back=stable_target(rows,prior,'settings',(.015,.23),(.025,.12))
            if back:
                self.settings_back=True
                return tap(back.x,back.y),'back to stock Settings main list'
        if pg=='cellular_pane':
            self.info.update(native_cellular_row_visible=True,native_cellular_pane_visible=True,
                             pane_frame=frame,pane_frame_sha256=sha)
            return self.done('stock cellular pane observed')
        if pg=='settings':
            row=stable_target(rows,prior,'cellular',(.07,.60),(.14,.94))
            bounds=wireless_bounds(rows)
            previous_bounds=wireless_bounds(prior)
            qualified=(row is not None and bounds and previous_bounds and
                       bounds[0]+.01<row.y<bounds[1]-.01 and previous_bounds[0]+.01<row.y<previous_bounds[1]-.01)
            if qualified and not self.cellular_tap:
                self.cellular_tap=True
                self.info.update(native_cellular_row_visible=True,row_frame=frame,row_frame_sha256=sha)
                return tap(row.x,row.y),'visible stock Cellular navigation label'
            if self.cellular_tap:return None
            if row is None and not any(m.kind=='cellular' for m in rows+prior) and bounds and previous_bounds:
                self.info.update(wireless_group_bracketed=True,wireless_frame=frame,wireless_frame_sha256=sha)
                return self.done('whole wireless group captured for visual inspection')
            if self.settings_scrolls<3:
                self.settings_scrolls+=1
                return swipe(.50,.305,.50,.795),'scroll stock Settings toward wireless group'
        return None

def active_ticket(ticket):
    queue=Path(os.environ.get('BOOTQ_DIR','/tmp/hackios-bootq'))/f'ticket-{ticket}.json'
    try:record=json.loads(queue.read_text())
    except (OSError,ValueError):return False
    return record.get('id')==ticket and record.get('state')=='running'

def completed_since(path,not_before):
    # Run names encode HHMMSS and can repeat on another day. An older final
    # file cannot terminate or preclude the current ticket's observation.
    try:return path.stat().st_mtime>=not_before-1
    except FileNotFoundError:return False

def watch(ticket,folder,not_before,seconds,requested,observe_online=False,online_earliest_fallback=195):
    folder.mkdir(parents=True,exist_ok=True)
    journal=folder/'activation-ui.json';route=Route(ticket,observe_online);data=route.info
    if observe_online:data['observation_goal']='cold subscription and actual radio state'
    if journal.exists():raise SystemExit('existing journal; refusing to duplicate UI gestures')
    write(journal,data);deadline=time.monotonic()+seconds;boot=None;agent=False
    while time.monotonic()<deadline and active_ticket(ticket):
        if boot is None:
            found=current_run(not_before,requested)
            if found is None:time.sleep(1);continue
            boot,epoch=found
            if observe_online:
                # The authorized ONLINE decision can be intentionally late. A quick
                # Home launch must not leave Settings as the foreground on that boundary.
                route.online_fallback_not_before=time.monotonic()+max(0.,epoch+online_earliest_fallback-time.time())
                data['earliest_unresolved_fallback_kernel_seconds']=online_earliest_fallback
            data['run']=boot;write(journal,data)
        if completed_since(K/'logs'/f'{boot}-run.log',not_before):break
        frame=folder/f'cellular-{boot}-{time.monotonic_ns()}.png'
        try:
            p=subprocess.run(['bash','-ec','. common/lib.sh; load_device instantnoodle; ./xl screenshot "$1"','bash',str(frame)],
                             cwd=K,capture_output=True,timeout=20)
            if p.returncode or not frame.is_file():time.sleep(1);continue
            rows=recognize(frame)
        except (OSError,ValueError,subprocess.SubprocessError):
            time.sleep(1);continue
        sha=hashlib.sha256(frame.read_bytes()).hexdigest()
        action=route.decide(time.monotonic(),rows,frame.name,sha)
        write(journal,data)
        if route.finished:break
        if action:
            cmd,reason=action
            if not active_ticket(ticket) or completed_since(K/'logs'/f'{boot}-run.log',not_before):break
            if not agent:
                answer=command(('agent',));props=answer.get('result',{}).get('data',{})
                if not answer.get('ok') or not props.get('control') or props.get('userspace'):
                    route.done('kernel input gate unavailable');break
                agent=True
            answer=command(cmd)
            record={'command':cmd[0],'target':reason,'frame':frame.name,'sha256':sha,'ack':answer.get('ok') is True}
            if not record['ack']:
                record['error']=answer.get('error',{}).get('code','unconfirmed_delivery')
            data['actions'].append(record)
            if not answer.get('ok'):
                route.done('unconfirmed navigation action; no replay');break
            route.last_page=None;route.last_rows=[];write(journal,data);time.sleep(.7)
        time.sleep(.5)
    if not route.finished:
        data['coverage']='window ended before complete visual verification'
    write(journal,data)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames',required=True,type=Path)
    parser.add_argument('--not-before',required=True,type=float)
    parser.add_argument('--seconds',type=int,default=270)
    parser.add_argument('--run-id',default=None)
    parser.add_argument('--observe-online',action='store_true')
    parser.add_argument('--online-earliest-fallback',type=int,default=195)
    args=parser.parse_args()
    ticket=os.environ.get('BOOTQ_TICKET','')
    if not ticket.isdecimal() or not active_ticket(int(ticket)):parser.error('requires current cellular operator ticket')
    if not 0<=args.online_earliest_fallback<=600:parser.error('invalid fallback bound')
    if args.run_id and not re.fullmatch(r'[0-9]{6}',args.run_id):parser.error('invalid live run')
    if not SCREEN.is_file():parser.error('offline OCR executable missing; run build_ocr.py')
    os.umask(0o077)
    watch(int(ticket),args.frames,args.not_before,args.seconds,args.run_id,args.observe_online,args.online_earliest_fallback)
