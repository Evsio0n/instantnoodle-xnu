"""Summarize captured monitor frames only. Does not open the phone or issue a command."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

def summarize(folder,ticket):
    receipt=json.loads((folder/'activation-ui.json').read_text())
    run=receipt.get('run','')
    if receipt.get('ticket')!=ticket or not re.fullmatch(r'\d{6}',run):
        raise ValueError('no attributable ticket/run')
    timeline=[];prior_phase=None;known={};last_live=None;last=None;first_live=None;first_radio=None
    for item in receipt['captured_app_frames']:
        basename=item['file']
        if not re.fullmatch(r'cellular-'+run+r'-[0-9]+\.png',basename):
            raise ValueError('unexpected capture path')
        f=folder/basename
        if hashlib.sha256(f.read_bytes()).hexdigest()!=item['sha256']:
            raise ValueError('capture failed its original hash')
        stat=f.stat()
        captured=datetime.fromtimestamp(stat.st_mtime,timezone.utc).astimezone().isoformat(timespec='milliseconds')
        evidence={'file':basename,'sha256':item['sha256'],'host_png_saved_at':captured,
                  'phase':item['phase'],'samples':item['samples'],'fields':item['projected_values'],'decisions':item['decisions']}
        phase=tuple(item['phase'])
        if phase and phase!=prior_phase:
            timeline.append(dict(evidence,change='visible phase'))
            known={}
            prior_phase=phase
        elif phase:
            changed={k:(known[k],v) for k,v in item['projected_values'].items() if k in known and v!=known[k]}
            if changed:timeline.append(dict(evidence,change='readable field changed',changed=changed))
        if phase:known.update(item['projected_values'])
        last=evidence
        if 'live' not in item['phase'] or not item['samples']:continue
        if first_live is None:first_live=evidence
        if first_radio is None and ('dms_data'in item['projected_values'] or 'nas_data'in item['projected_values']):first_radio=evidence
        last_live=evidence
    return {'ticket':ticket,'run':run,'total_app_frames':len(receipt['captured_app_frames']),
            'first_live_frame':first_live,'first_live_radio_frame':first_radio,'last_live_frame':last_live,
            'observed_changes':timeline,'last_app_frame':last,
            'coverage':receipt['coverage'],'captured_time_basis':'host PNG modification time; displayed age is the modem owner clock'}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);p.add_argument('--ticket',required=True,type=int)
    args=p.parse_args();print(json.dumps(summarize(args.folder,args.ticket),ensure_ascii=False,indent=2))
