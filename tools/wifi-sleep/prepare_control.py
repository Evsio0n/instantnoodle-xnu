#!/usr/bin/env python3
"""Prepare a new, isolated Wi-Fi-OFF sleep control; never starts the phone.
Only publishes a page row after image construction and effective-argument checks.
Does not enable the known-failing Wi-Fi-on sleep path.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

K = Path(__file__).resolve().parents[2]
LONG = '--long' in sys.argv
NAME = 'wifi-sleep-control-30s-20261106' if LONG else 'wifi-sleep-control-v5-20261106'
OUT = K / 'out' / NAME
ROOT = OUT / 'root'
IMAGE = OUT / 'root.apfs'
KERNEL = K / 'out/kernels/mdp-93fe105e.mach'
RUNNER = K.parent / 'oneplus8-audit/camera-native/run_cam_probe.py'
FLAGS = ['lean', 'ramhfs', 'datadefault', 'nowifi', 'nosound', 'nocam', 'silent',
         'bt=0', 'torch=0', 'modem=0', 'slpi_boot_once=0', 'mic=0', 'sleepon', 'sleep_max=15', 'secs=210']
if LONG:
    FLAGS = [f.replace('sleep_max=15', 'sleep_max=40').replace('secs=210', 'secs=300') for f in FLAGS]
JOBS = '''# One helper checks each prerequisite; rc.jobs itself does not fail-fast.
/hackios/bin/wifi_sleep_control
/hackios/bin/rwtouch mark done
'''

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def main():
    # Fail rather than overwrite a prepared or running experiment.
    if OUT.exists():
        raise SystemExit(f'already exists: {OUT}; inspect it, do not overwrite')
    env = dict(os.environ, CAM_KERNEL=str(KERNEL), CAM_IMAGE=str(IMAGE))
    preview = subprocess.run([sys.executable, str(RUNNER), '5', '', *FLAGS, 'dryrun'],
                             cwd=K, env=env, check=True, capture_output=True, text=True)
    plan = json.loads(preview.stdout)
    pairs = [word.split('=', 1) for word in plan['args'].split() if '=' in word]
    assert len(pairs) == len({k for k, v in pairs}), 'duplicate boot argument keys'
    args = dict(pairs)
    assert sha(KERNEL) == '93fe105edda98f53bc62007ebf24b7b7e73306b5bfd235b97a7c1f53ca056d10'
    assert args.get('syssleep', '1') == '1' and args.get('gpu_sleep', '1') == '1'
    required = dict(wifi='0', adsp='0', sound='0', haptic='0', cam='0', sound_amps='0',
                    bt='0', torch='0', modem='0', slpi_boot_once='0', mic='0', ufs_write='0', var_store='0', hk_data='2')
    assert all(args.get(k) == v for k, v in required.items()), 'unsafe effective boot arguments'
    assert 'syssleep=0' not in plan['args'] and 'gpu_sleep=0' not in plan['args']
    assert plan['command_bytes'] <= 607
    OUT.mkdir(mode=0o700)
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios16.5', '-arch', 'arm64',
                    '-isysroot', str(K / 'local-theos/sdks/iPhoneOS16.5.sdk'), '-O1',
                    '-Wall', '-Wextra', *(['-DCONTROL_SLEEP="30"', '-DCONTROL_OBSERVE="60"'] if LONG else []), '-o', str(OUT / 'wifi_sleep_control'),
                    str(K / 'tools/wifi-sleep/control.c')], cwd=K, check=True)
    subprocess.run(['codesign', '--sign', '-', str(OUT / 'wifi_sleep_control')], check=True)
    subprocess.run(['codesign', '--verify', str(OUT / 'wifi_sleep_control')], check=True)
    # Do not mutate shared current-root or import the owner's photo-containing roots.
    def ignore(directory, names):
        return [n for n in names if n in ('import', 'network') or n.endswith(('.heic', '.HEIC', '.jpg', '.png'))]
    shutil.copytree(K / 'out/current-root', ROOT, symlinks=True, ignore=ignore)
    links = {str(p.relative_to(ROOT)): os.readlink(p) for p in ROOT.rglob('*') if p.is_symlink()}
    assert links == {'hal/HKAudio.driver': '/hackios/lib/HKAudio.driver',
                     'ext/HKMetal.bundle': '/hackios/lib/HKMetal.bundle'}, 'unreviewed root symlinks'
    for name in ('power_test', 'rwtouch'):
        assert (ROOT / 'bin' / name).is_file()
    assert not (ROOT / 'import').exists() and not (ROOT / 'fw/network').exists()
    shutil.copy2(OUT / 'wifi_sleep_control', ROOT / 'bin/wifi_sleep_control')
    (ROOT / 'bin/wifi_sleep_control').chmod(0o755)
    (ROOT / 'etc/rc.jobs').write_text(JOBS)
    subprocess.run([sys.executable, 'tools/apfs/mkapfs.py', '-z', '--plain', 'fw',
                    '-o', str(IMAGE), f'hackios={ROOT}'], cwd=K, check=True)
    IMAGE.chmod(0o600)
    receipt = dict(plan, kind='wifi-OFF sleep control, NOT Wi-Fi-resume proof',
                   image_sha256=sha(IMAGE), kernel_sha256=sha(KERNEL),
                   jobs_sha256=hashlib.sha256(JOBS.encode()).hexdigest())
    (OUT / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    versions = K / 'out/eye/versions.json'
    rows = json.loads(versions.read_text())
    assert not any(row['id'] == NAME for row in rows)
    row = dict(id=NAME, title='睡眠對照：Wi-Fi 關閉，只睡一次 5 秒',
        why='先驗證測試管線，不是 Wi-Fi 睡醒修復版。RAM /var、不寫 UFS、不匯入照片；Wi-Fi、藍牙、聲音、相機、閃光燈、modem、SLPI 關閉。\n'
            '開機後先不要操作：系統約第 60 秒後自動鎖屏，睡 5 秒，定時喚醒。請看螢幕是否重新亮起、觸控是否可用，並在備註填結果。\n'
            '如果沒回來，停止測試，按住 Power＋音量上恢復，再進 USB Developer mode。不要重複點擊。\n'
            '目前只開放這個對照，Wi-Fi 開啟的睡眠仍待修復與安全門檻。',
        root=str(IMAGE.relative_to(K)), kernel=str(KERNEL.relative_to(K)), flags=FLAGS)
    if LONG:
        row['title'] = '睡眠對照加長：Wi-Fi 關閉，睡 30 秒＋醒後觀察 60 秒'
        row['why'] = row['why'].replace('睡 5 秒', '睡 30 秒').replace('系統約第 60 秒後', '主畫面穩定後') + '\n醒來後保留 60 秒供你觀察及操作，之後自動回 loader。整輪最多 300 秒。'
    # Keep previous rows and a reversible local snapshot; live server reloads this JSON.
    (OUT / 'versions-before.json').write_text(versions.read_text())
    temp = OUT / 'versions-new.json'
    temp.write_text(json.dumps([row, *rows], ensure_ascii=False, indent=2) + '\n')
    os.replace(temp, versions)
    print(json.dumps({k: receipt[k] for k in ('image_sha256', 'kernel_sha256', 'command_bytes')}, indent=2))

if __name__ == '__main__':
    main()
