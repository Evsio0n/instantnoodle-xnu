"""Run the additive GNSS bundle and check the original app library on IN2010."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

import gnss_fix_ticket as base
import gnss_settings_gate

MANIFEST = Path(__file__).with_name('gnss_settings_profile.json')
VIEW = re.compile(rb'(?m)^app-view: stock_before=(\d+) stock_after=(\d+) settings_same=([01]) owned_bundle=([01])$')
ENUM = re.compile(rb'stock-apps: enum uid=501 bulk_base=(\d+) bulk_staged=(\d+) bulk_dup=(\d+) bulk_errno=(\d+) '
                  rb'plain_base=(\d+) plain_staged=(\d+) plain_dup=(\d+) plain_errno=(\d+) end')
CONTROL = re.compile(rb'stock-apps: result uid=501 controls=(\d+) total=(\d+) pass=([01]) end')


def spec():
    manifest, kernel, root, size = base.spec(MANIFEST, 'gnss-stock-apps')
    if (manifest['reference_ticket'] != 237 or manifest['reference_run'] != '060754'
            or manifest['normal_development_signing'] is not True
            or manifest['stock_helper_sha256'] != '6c22ad19a13fa503276d10711271f9251f6d1b21a207df58be9f710e608b789f'
            or manifest['bundle_cdhash'] != '9b6ef17114656071e74b3f8da2c490d7591f852c'):
        raise ValueError('wrong application staging profile')
    return manifest, kernel, root, size


def stock_evidence(files):
    views, inventories, controls = set(), set(), set()
    for path in files:
        _, marker, body = path.read_bytes().rpartition(b'== KERNEL LOG (the kernel is still running) ==')
        if not marker:
            continue
        for rx, observed in ((VIEW,views),(ENUM,inventories),(CONTROL,controls)):
            observed.update(tuple(map(int,m.groups())) for m in rx.finditer(body))
    return dict(init_app_views=sorted(views), mobile_stock_inventories=sorted(inventories),
                mobile_stock_controls=sorted(controls),
                original_directory_preserved=bool(views) and all(a==b and a>=173 and c==d==1 for a,b,c,d in views),
                original_names_visible=(141,32,0,0,141,32,0,0) in inventories,
                stock_controls_verified=(7,7,1) in controls)


def collect(files, manifest):
    facts = gnss_settings_gate.collect(files, manifest)
    facts.update(stock_evidence(files))
    return facts


def complete(f):
    return all(f.get(k) for k in ('clean_return','teardown','pins_down','bundle_matches','bundle_visible',
        'original_directory_preserved','original_names_visible','stock_controls_verified',
        'settings_bundle_on_disk','settings_proxy_registered','signed_app_registered'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--run', action='store_true')
    group.add_argument('--inside-ticket', action='store_true')
    opts=parser.parse_args()
    manifest,kernel,root,size=spec()
    print('additive app profile',manifest['root_sha256'][:12],manifest['bundle_cdhash'][:12],size,'bytes',flush=True)
    if opts.inside_ticket:
        return base.inside(manifest,kernel,root,folder_prefix='gnss-stock-apps',collect=collect,evaluate=complete)
    if opts.run:
        if 'BOOTQ_TICKET' in os.environ:
            raise RuntimeError('submit outside an active ticket')
        worker=shlex.join([sys.executable,str(Path(__file__).resolve()),'--inside-ticket'])
        return subprocess.run([str(base.ROOT/'tools/bootq'),'run','--who','gnss','--what',
                               'Verify GNSS Monitor with full stock app library','--max','1150','--',worker],
                              cwd=base.ROOT).returncode
    return 0


if __name__=='__main__':
    sys.exit(main())
