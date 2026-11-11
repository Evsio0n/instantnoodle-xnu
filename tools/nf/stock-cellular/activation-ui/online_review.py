"""Summarize already captured same-boot UI frames; no USB, HTTP or modem operation."""
import argparse
import hashlib
import json
from pathlib import Path
import re

def frame(directory,name,expected):
    if not name or Path(name).name!=name:return None
    target=directory/name
    if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest()!=expected:
        raise ValueError('Missing or changed frame '+name)
    return dict(file=name,sha256=expected)

def summarize(folder,ticket):
    j=json.loads((folder/'activation-ui.json').read_text())
    if j.get('ticket')!=ticket or not re.fullmatch(r'\d{6}',str(j.get('run',''))):
        raise ValueError('Wrong ticket or missing device run')
    if j.get('observation_goal')!='cold subscription and actual radio state':
        raise ValueError('Wrong visual observation mode')
    rows=j['captured_app_frames']
    for item in rows:
        if not item['file'].startswith('cellular-'+j['run']+'-'):
            raise ValueError('Frame from a different run')
    live=[r for r in rows if 'live' in r['phase'] and r['samples']]
    def qualification(row):
        data=row['projected_values']
        return (data.get('dms_data')=='dms online',data.get('nas_data')=='nas registered',
                data.get('ps_attach')=='ps attached',data.get('cs_attach')=='cs attached',
                data.get('gw_data')=='mapped', data.get('usim_state')=='usim ready',
                'wds_data'in data,len(data))
    # Positive evidence must come from one actual frame. Never join fields
    # from different moments into a fabricated successful screen.
    best=max(live,key=qualification,default=None)
    if best:
        best_image=frame(folder,best['file'],best['sha256'])
        best_image.update(samples=best['samples'],phase=best['phase'],values=best['projected_values'],decisions=best['decisions'])
    else:best_image=None
    native_image=frame(folder,j.get('wireless_frame'),j.get('wireless_frame_sha256')) if j.get('wireless_group_bracketed') else None
    row_image=frame(folder,j.get('row_frame'),j.get('row_frame_sha256'))
    pane_image=frame(folder,j.get('pane_frame'),j.get('pane_frame_sha256'))
    return dict(ticket=ticket,run=j['run'],project_app_visible=bool(j.get('project_app_visible')),
                valid_live_frames=len(live),best_live_frame=best_image,
                observed_dms=sorted({r['projected_values']['dms_data'] for r in live if 'dms_data' in r['projected_values']}),
                observed_nas=sorted({r['projected_values']['nas_data'] for r in live if 'nas_data' in r['projected_values']}),
                wireless_group_frame=native_image,cellular_row_frame=row_image,cellular_pane_frame=pane_image,
                native_row_absence_requires_visual_review=bool(native_image and not row_image),
                roaming_in_app=None,visual_coverage=j['coverage'])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);p.add_argument('--ticket',type=int,required=True)
    a=p.parse_args();print(json.dumps(summarize(a.folder,a.ticket),indent=2,ensure_ascii=False))
