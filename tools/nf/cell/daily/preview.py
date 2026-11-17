"""Owner-authorized preview using the existing writable HFS data, no bundle write."""
import os,subprocess
from pathlib import Path
K=Path(__file__).resolve().parents[4];os.chdir(K);O=K/'out/daily-20261115'
line=(O/'cmdline.txt').read_text().split();line=['var_store=0' if x=='var_store=1' else x for x in line];line+=['iphont=1','iphont_control=1','sound_amps=0']
assert 'hk_data=1' in line and 'ufs_write=1' in line and not any(x.startswith('var_data_format=') for x in line)
args=' '.join(line);seconds=180;full=f'loader_return_after={seconds} epoch={int(__import__("time").time())} sb_delay=8 '+args
assert len(full)+Path('../oneplus8-audit/gpu-plan/observed-loader-prefix.txt').stat().st_size<=607
(O/'preview-cmdline.txt').write_text(args+'\n')
env=dict(os.environ,XL_KERNEL=(O/'kernel.txt').read_text().strip(),XL_RAMDISK=str(O/'root.apfs'),XL_SECS=str(seconds),XL_FOLLOW='2',XL_FOLLOW_KEEP='',XL_SB_DELAY='8',XL_CMDLINE=args,PATH='/opt/homebrew/opt/coreutils/libexec/gnubin:'+os.environ['PATH'])
subprocess.run(['tools/bootq','run','--who','daily','--what','owner-authorized daily preview on kept HFS data; no format, no bundle writes','--max','600','--','bash tools/nf/cell/inner.sh > logs/daily-20261115-preview.out 2>&1'],env=env,check=True)
