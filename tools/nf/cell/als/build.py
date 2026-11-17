#!/usr/bin/env python3
"""Relink the installed cellular kernel in a private directory; never update its pins."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
K = HERE.parents[3]
BASE_COMMIT = 'e33c90b'  # Public baseline; tools/nf/cell is byte-identical to the original 819b553 tree.
CELL = 'tools/nf/cell'
BASE_SHA = '0a6636902ab9411bf45cf7767c994a0e77f1061412c25bd393538304cce0fa08'
SOURCE = Path(os.environ.get('HACKIOS_ALS_SOURCE', str(HERE / 'base')))


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def replace_once(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before, after)


def main(argv=None, *, extend_private_link=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--control', action='store_true')
    mode.add_argument('--settings', action='store_true')
    parser.add_argument('--sim-state-patch', type=Path,
                        help='Apply the SIM-state producer patch to the archived cellular driver')
    parser.add_argument('--output', type=Path,
                        help='Write a private candidate pin instead of updating the normal feature pin')
    args = parser.parse_args(argv)
    control, settings = args.control, args.settings
    if control and args.sim_state_patch:
        parser.error('the control build must use the original driver')
    assert digest(Path((K / 'out/cell-kernel-installed.txt').read_text().strip())) == BASE_SHA
    with tempfile.TemporaryDirectory(prefix='cell-als-') as temp:
        root = Path(temp)
        raw = subprocess.check_output(['git', 'archive', BASE_COMMIT, CELL], cwd=K)
        with tarfile.open(fileobj=io.BytesIO(raw)) as bundle:
            bundle.extractall(root, filter='data')
        cell = root / CELL
        if args.sim_state_patch:
            subprocess.run(['patch', '-s', '-p1', '-i', str(args.sim_state_patch.resolve())],
                           cwd=cell, check=True)
        builder = cell / 'build.py'
        text = builder.read_text()
        text = replace_once(text, 'else HERE.parents[2]', f'else Path({str(K)!r})')
        # The baseline linker normally writes the daily pointer; use our own pin for both builds.
        text = replace_once(text, "KERNEL / 'out/cell-kernel.txt'", f"Path({str(root / 'result')!r})")
        log = 'cell-als-control.log' if control else 'cell-als-build.log'
        if settings:
            log = 'cell-als-settings-build.log'
        text = replace_once(text, "'cell-build.log'", repr(log))
        if not control:
            manifest = json.loads((HERE / 'sources.json').read_text())
            sensor = cell / 'overlay/xnu/files/bsd/dev'
            display = cell / 'overlay/xnu/files/iokit/Kernel'
            display.mkdir(parents=True, exist_ok=True)
            for name, sha in manifest.items():
                source = SOURCE / name
                if name == 'MiniAppleARMBacklight.cpp':
                    source=K/'xnu/files/iokit/Kernel'/name
                elif name == 'slpi_dma_route.h':
                    source = K / 'xnu/files/bsd/dev' / name
                elif name == 'hk_adsp.c':
                    source = sensor / name
                assert digest(source) == sha, name
                dest = display if name in ('MiniAppleARMBacklight.cpp','MiniIOMobileFramebuffer.cpp', 'hk_light_policy.h', 'hk_user_preference.h', 'lp_tables.h', 'panel_trial.inc') else sensor
                if source != dest / name:
                    shutil.copyfile(source, dest / name)
            for d in (sensor, display):
                patch = HERE / ('display.patch' if d == display else 'sensor.patch')
                subprocess.run(['patch', '-s', '-p1', '-i', str(patch)], cwd=d, check=True)
            if settings:
                subprocess.run(['patch', '-s', '-p1', '-i', str(HERE / 'registry.patch')], cwd=sensor, check=True)
                shutil.copyfile(HERE / 'HKFrontLightSensor.cpp', cell / 'HKFrontLightSensor.cpp')
                text = replace_once(text, 'def run(*cmd, **kw):', '''DRIVER += ('HKFrontLightSensor.cpp',)
NEW.append('HKFrontLightSensor')
MAY_APPEND.add('HKFrontLightSensor')
CXX['HKFrontLightSensor'] = 'tools/nf/cell/HKFrontLightSensor.cpp'


def run(*cmd, **kw):''')
            text = replace_once(text, "old = str(wd / (name + '.o'))", "old = str(wd / (name + ('.cpo' if name in ('MiniAppleARMBacklight','MiniIOMobileFramebuffer') else '.o')))")
            text = replace_once(text, 'src = stage / CXX[name]', '''src = stage / CXX[name]
            if name in ('MiniAppleARMBacklight','MiniIOMobileFramebuffer'):
                cc[1:1] = ['-I', str(nf / 'iokit/Kernel')]''')
            text = replace_once(text, 'def run(*cmd, **kw):', """UNITS += ['hk_slpi_boot', 'MiniIOMobileFramebuffer','MiniAppleARMBacklight']
CXX['MiniAppleARMBacklight'] = 'xnu/files/iokit/Kernel/MiniAppleARMBacklight.cpp'
CXX['MiniIOMobileFramebuffer'] = 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp'


def run(*cmd, **kw):""")
        if extend_private_link is not None:
            text = extend_private_link(cell, text)
        builder.write_text(text)
        subprocess.run([sys.executable, str(builder)], cwd=K, check=True)
        artifact = Path((root / 'result').read_text().strip())
        if control:
            assert digest(artifact) == BASE_SHA, 'unmatched control: ' + digest(artifact)
        else:
            assert (HERE / 'control.sha').read_text().strip() == BASE_SHA
            output = 'cell-als-settings-kernel.txt' if settings else 'cell-als-kernel.txt'
            if args.sim_state_patch:
                output = 'cell-settings-sim-kernel.txt' if settings else 'cell-sim-kernel.txt'
            pin = args.output.resolve() if args.output else K / 'out' / output
            pin.parent.mkdir(parents=True, exist_ok=True)
            pin.write_text(str(artifact) + '\n')
            if args.sim_state_patch:
                receipt = {
                    'kernel': str(artifact), 'kernel_sha256': digest(artifact),
                    'base_commit': BASE_COMMIT, 'base_kernel_sha256': BASE_SHA,
                    'settings_service': settings,
                    'sim_state_patch': str(args.sim_state_patch.resolve()),
                    'sim_state_patch_sha256': digest(args.sim_state_patch),
                    'sensor_patch_sha256': digest(HERE / 'sensor.patch'),
                    'display_patch_sha256': digest(HERE / 'display.patch'),
                    'cell_driver_sha256': digest(cell / 'instantnoodle_cell.c'),
                    'registry_patch_sha256': digest(HERE / 'registry.patch') if settings else None,
                    'front_light_service_sha256': digest(HERE / 'HKFrontLightSensor.cpp') if settings else None,
                    'ath11k_object': os.environ.get('CELL_ATH11K_OBJ'),
                    'ath11k_object_sha256': digest(Path(os.environ['CELL_ATH11K_OBJ'])) if os.environ.get('CELL_ATH11K_OBJ') else None,
                }
                pin.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if control:
            (HERE / 'control.sha').write_text(BASE_SHA + '\n')
        print('control' if control else 'ALS', artifact)

if __name__ == '__main__':
    main()
