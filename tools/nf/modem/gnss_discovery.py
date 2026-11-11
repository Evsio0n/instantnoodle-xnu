"""Read-only preflight by default. --run schedules one GNSS app-discovery RAM case."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

import gnss_fix_ticket as base

ROOT=base.ROOT
MANIFEST=Path(__file__).with_name('gnss_discovery_profile.json')
LINE=re.compile(rb'gnss-ls seq=(\d+) uid=(\d+) settings=(-?\d+) id=(-?\d+) installed=(-?\d+) byurl=(-?\d+) path=([^ \r\n]+) exe=([0-9a-f]{64}|missing) plist=([0-9a-f]{64}|missing) end')
OWN=re.compile(r'(?:/Applications/GNSSMonitor\.app|(?:/private)?/var/containers/Bundle/Application/[0-9A-Fa-f-]{36}/GNSSMonitor\.app)')
def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def spec():
    baseline,kernel,_,n=base.spec()
    m=json.loads(MANIFEST.read_text())
    root=ROOT/'out'/f'gnss-discovery-{m["root_sha256"]}.apfs'
    if (digest(root)!=m['root_sha256'] or m['kernel_sha256']!=baseline['kernel_sha256'] or m['reference_run']!='052622'
        or m['reference_ticket']!=228 or m['normal_development_signing'] is not True or
        m['bundle_id']!=baseline['bundle_id'] or m['bundle_cdhash']==baseline['bundle_cdhash'] or
        not re.fullmatch('[a-f0-9]{64}',m['issuer_profile_sha256'])):raise ValueError('incorrect or changed discovery pins')
    return m,kernel,root,n
def collect(files,m):
    f=base.evidence(files,m)
    snapshots={};reason=False;url=False;manual=False;unavailable=False
    for path in files:
        raw=path.read_bytes()
        reason |= b'gnss-sbs original_code=9 reason=security policy error end' in raw
        url |= b'GNSSMonitor: own URL delivered by system' in raw
        unavailable |= b'gnss-ls-unavailable ' in raw
        # The new job never asks SBS to launch the app. Reject the obsolete worker.
        manual |= b'$ /hackios/bin/battery_test launch com.hackios.gnss-monitor' in raw
        for hit in LINE.finditer(raw):
            seq,uid,settings,present,installed,byurl=map(int,hit.groups()[:6])
            loc,exe,plist=(s.decode() for s in hit.groups()[6:])
            if not 0<=seq<7 or uid!=501 or settings not in (-1,0,1) or installed not in (-1,0,1) or present not in (0,1) or byurl not in (0,1):
                raise ValueError('invalid lookup record')
            if loc not in ('other','none') and not OWN.fullmatch(loc):raise ValueError('unrelated app path in record')
            row=dict(uid=uid,settings=bool(settings==1),identifier=bool(present),installed=installed,by_url=bool(byurl),path=loc,
                     signed_bundle_matches=exe==m['executable_sha256'] and plist==m['plist_sha256'])
            if seq in snapshots and row!=snapshots[seq]:raise ValueError('conflicting LaunchServices records')
            snapshots[seq]=row
    usable=[x for i,x in sorted(snapshots.items()) if x['settings']]
    last=usable[-1] if usable else None
    registered=bool(last and last['identifier'] and last['installed']==1 and last['signed_bundle_matches'] and OWN.fullmatch(last['path']))
    f.update(sbs_error_9_verified=reason,legacy_launch_requested=manual,ls_helper_unavailable=unavailable,
             ls_samples={str(i):r for i,r in sorted(snapshots.items())},ls_valid_controls=len(usable),
             normal_registered=registered,registered_path=last['path'] if registered else None,
             discovery_classification=('registered-and-matched' if registered else
                'no-registered-identifier' if last and not last['identifier'] else
                'different-or-incomplete-proxy' if last else 'lookup-inconclusive'),
             own_url_delivered=url,normal_native_foreground=registered and (f['app_foreground'] or f['scene_foreground']),
             device_provisioning_eligibility_verified=False)
    return f
def complete(f):
    return bool(f['clean_return'] and f['teardown'] and f['pins_down'] and f['bundle_matches'] and f['bundle_visible'] and
                f['sbs_error_9_verified'] and not f['legacy_launch_requested'] and f['ls_valid_controls']>=2 and
                f['quality_samples'] and f['loc_accepted'])
def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group()
    g.add_argument('--run',action='store_true');g.add_argument('--inside-ticket',action='store_true')
    a=p.parse_args()
    if a.run:
        return subprocess.run([sys.executable,str(Path(__file__).with_name('gnss_settings_ticket.py')),'--run'],cwd=ROOT).returncode
    if a.inside_ticket:
        raise RuntimeError('retired single-directory GNSS image; use gnss_settings_ticket.py')
    m,k,r,n=spec()
    print('GNSS discovery profile',m['kernel_sha256'][:12],m['root_sha256'][:12],m['bundle_cdhash'][:12],n,'argument bytes',flush=True)
    return 0
if __name__=='__main__':sys.exit(main())
