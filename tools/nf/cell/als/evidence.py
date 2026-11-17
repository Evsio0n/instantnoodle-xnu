#!/usr/bin/env python3
"""Summarize only qualified ALS, preference, panel and cleanup evidence in owned handset runs."""
from pathlib import Path
import json
import re
import struct
from statistics import median

K=Path(__file__).resolve().parents[4]
RUNS={395:('214818','first'),396:('215428','metadata'),397:('215836','calibrated'),398:('220654','enabled'),399:('221312','stock'),400:('222029','candidate'),401:('222646','idle'),402:('223327','session'),403:('072309','exposure'),404:('073355','smooth')}
SOURCE=re.compile(r'cell-als: front seq=(\d+) lux_bits=([a-f0-9]+) status=3 ticks=(\d+) path=1 factory=1 coefficient_served=([01])')
PREF=re.compile(r'light-pref: ordinary mobile (?:CoreBrightness DisplayBrightnessAuto|Settings _AXSAutoBrightnessEnabled) state=([012]) kernel_result=0')
CURVE=re.compile(r'light-curve: seq=(\d+) tick=(\d+) mlux=(\d+) nits_x10000=(\d+) reference_dbv=(\d+) goal_dbv=(\d+)')
SELECT=re.compile(r'light-panel: select seq=(\d+) tick=(\d+) us=\d+ lux_bits=[0-9a-f]+ band=\d+ from=(\d+) target=(\d+)')
APPLY=re.compile(r'light-panel: apply target=(\d+) before=(\d+) after=(\d+) dma_done=1 fresh_done=1')
FRESH=re.compile(r'light-source: gates fresh=(\d+) enabled=\d+ path_match=(\d+) factory_event=(\d+) panel_called=(\d+)')
ROUTE=re.compile(r'cell-als: route bank=(\d+) slot=(\d+) mapped=(\d+) reclaimed=(\d+)')
CLEAN=re.compile(r'slpi: ended; active=(\d+) floors-held=(\d+) poisoned=(\d+)')
META=re.compile(r'sensor-cali: super magic=([a-f0-9]+) state=([a-f0-9]+) block_log=\d+ rev=\d+ compat=([a-f0-9]+) incompat=([a-f0-9]+) ro_compat=([a-f0-9]+)')
UID=re.compile(r'slpi-wise: sent=1 accepted=1 rejected=0 invalid=0 samples=(\d+) malformed=(\d+) uid=([0-9a-f]+,[0-9a-f]+)')

RAMP=re.compile(r'light-ramp: us=(\d+) seq=(\d+) tick=(\d+) mlux=(\d+) goal=(\d+) from=(\d+) target=(\d+) before=(\d+) after=(\d+) dma_done=1 fresh_done=1')

def floats(h):
    return struct.unpack('>f',int(h,16).to_bytes(4,'big'))[0]

def ramp_timing(rows):
    if not rows or 'ap_us' not in rows[0]:
        return None
    # Compare only consecutive acknowledged hardware counters while pursuing
    # the same unfinished goal. Intervals across stationary periods are not cadence.
    intervals = [(a,b) for a,b in zip(rows,rows[1:])
                 if a['after']==b['before'] and a['to_dbv']==b['from_dbv']
                 and a['reference_dbv']==b['reference_dbv'] and a['to_dbv']!=a['reference_dbv']]
    durations=[b['ap_us']-a['ap_us'] for a,b in intervals]
    by_direction={}
    for direction in ['up','down']:
        steps=[abs(r['to_dbv']-r['from_dbv']) for r in rows if (r['to_dbv']>r['from_dbv'])==(direction=='up')]
        if steps:by_direction[direction]={'count':len(steps),'min_dbv':min(steps),'median_dbv':median(steps),'max_dbv':max(steps)}
    return {'directions':by_direction,'same_goal_contiguous_intervals':len(durations),
            'min_us':min(durations) if durations else None,'median_us':median(durations) if durations else None,
            'max_us':max(durations) if durations else None,'same_see_sample_subsequent_writes':sum(a['see_ticks']==b['see_ticks'] for a,b in intervals)}

def summarize(ticket, run, tag):
    directory=K/'logs'
    copies=sorted(directory.glob(run+'-run.log.live.[0-9]*'),key=lambda f:int(f.suffix[1:]))
    paths=copies+[directory/(run+'-run.log.live'),directory/(run+'-run.log'),K/'out'/f'cell-als-{tag}-full.txt']
    seen=set();samples={};pref=set();applied={};proved=set();curve_samples={}
    routed=clean=features=inventory=counts=None
    arms=set();system_owner=False;slider_holds=set();wakes=set();dark=False
    for path in paths:
        if not path.is_file():continue
        curve=None;selection=None
        for line in path.open(errors='replace'):
            if not any(s in line for s in ('cell-als:', 'light-', 'sensor-cali:', 'als-enable-', 'slpi-wise:', 'slpi-oem130:','slpi: ended','slpi-route: check=', 'display: the panel dark')):continue
            # A select/apply must be witnessed in order in the same captured file.
            if (m:=RAMP.search(line)):
                us,seq,tick,mlux,goal,old,new,before,after=map(int,m.groups())
                if after==before+1 and old!=new:
                    curve_samples[tick]=mlux
                    applied[us]={'ap_us':us,'sequence':seq,'see_ticks':tick,'observed_millilux':mlux,'reference_dbv':goal,'from_dbv':old,'to_dbv':new,'before':before,'after':after}
            elif (m:=CURVE.search(line)):
                curve=list(map(int,m.groups()));selection=None;curve_samples[curve[1]]=curve[2]
            elif (m:=SELECT.search(line)):
                selection=list(map(int,m.groups())) if curve and tuple(curve[:2])==tuple(map(int,m.groups()[:2])) else None
            elif (m:=APPLY.search(line)):
                target,before,after=map(int,m.groups())
                if curve and selection and target==selection[-1] and after==before+1:
                    applied[curve[1]]={'sequence':curve[0],'see_ticks':curve[1],'observed_millilux':curve[2],'reference_dbv':curve[4],'from_dbv':selection[-2],'to_dbv':target}
                curve=selection=None
            # Other fields have a unique value or are indexed by SEE timestamp.
            elif (m:=SOURCE.search(line)):
                n,h,t,cal=m.groups();samples[int(t)]=(int(n),floats(h),int(cal))
            elif (m:=PREF.search(line)):pref.add(int(m[1]))
            elif (m:=FRESH.search(line)):counts=dict(zip(('fresh','path','factory','panel_calls'),map(int,m.groups())))
            elif (m:=ROUTE.search(line)):routed=dict(zip(('bank','slot','mapped','reclaimed'),map(int,m.groups())))
            elif (m:=CLEAN.search(line)):clean=dict(zip(('dsp_active','floors_retained','lifecycle_poisoned'),map(int,m.groups())))
            elif (m:=META.search(line)):features=dict(zip(('magic','state','compat','incompat','ro_compat'),m.groups()))
            elif (m:=UID.search(line)):inventory={'accepted':int(m[1]),'malformed':int(m[2]),'source_uid':m[3]}
            elif (m:=re.search(r'light-panel: arm baseline=(\d+) source=(\d+) owner_gen=(\d+)',line)):arms.add(tuple(map(int,m.groups())))
            elif 'light-panel: system-owner' in line:system_owner=True
            elif (m:=re.search(r'light-panel: (?:manual-priority slider_q16|slider-owner current-lit-session q16)=(\d+)',line)):slider_holds.add(int(m[1]))
            elif 'light-panel: wake; waiting for fresh OS owner' in line or 'light-panel: wake prior_slider_hold=' in line:wakes.add('observed')
            elif 'display: the panel dark (1 command' in line:dark=True
            elif 'setter_reply=1 fresh_readback_on=0' in line:proved.add('generic_setter_accepted_readback_off')
            elif 'writes=1 stock_Settings_readback=on' in line:proved.add('stock_setter_readback_on')
            elif 'writes=0 Settings already on' in line:proved.add('stock_settings_already_on')
            elif 'current_extent_bytes=4 archived_receipt=249 content_and_blocks_match=1' in line:proved.add('current_bytes_match_ticket_249')
            elif 'slpi-oem130: restore ok=1 scoped_words=17 owned=0' in line:proved.add('all_17_board_words_released')
            elif 'slpi-route: check=0 want=0 got=0 rollback=1024 want=0 got=1f300' in line:proved.add('inert_CB9_type_retained')
            elif 'sensor-cali: read refused' in line:proved.add('factory_file_refused')
        seen.add(path.name)
    receipt=None
    for line in Path('/tmp/hackios-bootq/history.jsonl').read_text().splitlines():
        row=json.loads(line)
        if row.get('id')==ticket and row.get('state')=='ended':receipt=row.get('rc')
    last=sorted(samples.values());steps=list(dict(sorted(applied.items())).values())
    owner_report={
        403: 'Front light changes brightened and dimmed the screen. User exercised the manual slider and saw steps during automatic transitions.',
        404: 'User reported automatic transitions clearly smoother, without visible steps.'
    }.get(ticket)
    return {'owner_report':owner_report,'ticket':ticket,'run':run,'bootq_exit':receipt,'preference_source':('generic CoreBrightness DisplayBrightnessAuto' if ticket<=398 else 'stock Settings _AXSAutoBrightnessEnabled'),'preference_states_observed':sorted(pref),'confirmed_native_settings_on':ticket>=399 and 2 in pref,
            'observed_front_snapshots':len(last),'sparse_front_snapshot_lux_range':[min(x[1] for x in last),max(x[1] for x in last)] if last else None,
            'distinct_logged_curve_samples':len(curve_samples),'logged_curve_sample_lux_range':[min(curve_samples.values())/1000,max(curve_samples.values())/1000] if curve_samples else None,
            'snapshots_with_coefficient_served':sum(x[2] for x in last),'source_totals':inventory,'gate_totals':counts,
            'panel_arms':[{'baseline_dbv':v,'source':src,'owner_generation':gen} for v,src,gen in sorted(arms)],
            'observed_system_fade_release':system_owner,'observed_dark':dark,'observed_fresh_wake':bool(wakes),'manual_hold_q16':sorted(slider_holds),
            'confirmed_panel_steps':steps,'ramp_timing':ramp_timing(steps),'evidence':sorted(proved),
            'ext4':features,'route':routed,'shutdown':clean,'capture_files':len(seen)}

if __name__=='__main__':
    output=K/'out/cell-als-bringup-evidence.json'
    output.write_text(json.dumps([summarize(i,*pair) for i,pair in RUNS.items()],indent=2)+'\n')
    print(output)
