"""Compile and check the maintained read-only frontend, without operating a phone."""
import hashlib,json,subprocess
from pathlib import Path
H=Path(__file__).resolve().parent;K=H.parents[2]
OUT=K.parent/'oneplus8-audit/stock-cellular-ui/qmux-frontend';OUT.mkdir(parents=True,exist_ok=True)
c='oneplus8-build';remote=subprocess.check_output(['docker','exec',c,'mktemp','-d','/tmp/qmux-front-XXXXXXXX'],text=True).strip();assert remote.startswith('/tmp/qmux-front-')
script=r'''
. /build/xnu-env.sh
python3 - "$1" <<'INNER'
import shlex,shutil,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);base=Path('/build/xnu-src/nf-tree/xnu-8796.121.2');obj=base/'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837';source=Path('/build/xnu/files')

for name,unit in [('hk_qmux_frontend','bsd'),('HKCellularQMUX','iokit')]:
    wd=obj/unit/'DEVELOPMENT';cpp=unit=='iokit'
    opt=(wd/('.CXXFLAGS' if cpp else '.CFLAGS')).read_bytes().strip(b'\0\r\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)')
    cc=shlex.split(opt)
    if cpp:
        common=(wd/'.CFLAGS').read_bytes().strip(b'\0\r\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)')
        cc+=shlex.split(common)[1:]
    cc[1:1]=['-I',str(source/'bsd')] if cpp else ['-I',str(source/'bsd'),'-I',str(base/'bsd')]
    src=source/('iokit/Kernel' if cpp else 'bsd/dev')/(name+('.cpp' if cpp else '.c'))
    subprocess.run(cc+['-c',str(src),'-o',str(out/(name+'.o'))],cwd=wd,check=True)
INNER
'''
try:
    with (OUT/'objects.log').open('w') as log:
        subprocess.run(['docker','exec','-i','-e','THEOS=/build/local-theos',c,'bash','-se','--',remote],input=script,text=True,check=True,stdout=log,stderr=subprocess.STDOUT)
    files={}
    for name in ('hk_qmux_frontend','HKCellularQMUX'):
        f=OUT/(name+'.o');subprocess.run(['docker','cp',c+':'+remote+'/'+f.name,str(f)],check=True)
        files[f.name]=hashlib.sha256(f.read_bytes()).hexdigest()
    (OUT/'objects.json').write_text(json.dumps(files,indent=2)+'\n');print(files)
finally:subprocess.run(['docker','exec',c,'rm','-rf','--',remote],check=True)
