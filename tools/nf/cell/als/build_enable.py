#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
import hashlib
import shutil
import sys
HERE=Path(__file__).resolve().parent
K=HERE.parents[3]
C='oneplus8-build'
choices={'--stock':('stock_enable_once.m','enable-stock'), '--stock-bridge':('stock_pref_bridge.m','pref-stock')}
if len(sys.argv)>1 and sys.argv[1] not in choices: raise SystemExit('use --stock or --stock-bridge')
source,prefix=choices[sys.argv[1]] if len(sys.argv)>1 else ('enable_once.m','enable-once')
w=subprocess.check_output(['docker','exec',C,'mktemp','-d','/tmp/cell-als-enable-XXXXXXXX'],text=True).strip()
assert w.startswith('/tmp/cell-als-enable-')
try:
    subprocess.run(['docker','cp',str(HERE/source),C+':'+w+'/enable.m'],check=True)
    subprocess.run(['docker','exec','-w',w,C,'bash','-ec',
        'source /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework CoreFoundation enable.m -o enable_once'],check=True)
    with tempfile.TemporaryDirectory() as d:
        p=Path(d)/'enable_once'
        subprocess.run(['docker','cp',C+':'+w+'/enable_once',str(p)],check=True)
        output=K/'out'/('cell-als-'+prefix+'-'+hashlib.sha256(p.read_bytes()).hexdigest())
        if not output.exists():
            shutil.copyfile(p,output);output.chmod(0o555)
        print(output)
finally:
    subprocess.run(['docker','exec',C,'rm','-rf','--',w],check=True)
