"""Read-only inspection of native DSP firmware for requested listener module names."""
import hashlib,json,os,re,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT.parent/'oneplus8-audit/firmware-reader'))
from native_fetch import NativeReader

def main():
    if not os.environ.get("BOOTQ_TICKET","").isdigit(): raise RuntimeError("requires a bootq ticket")
    out=ROOT/'out'/'slpi-dsp-firmware';out.mkdir(mode=0o700,exist_ok=True)
    dev=NativeReader('xnuloader')
    status=dev.command('getvar','result')
    if not re.search(r'^result: idle\s*$',status,re.M):raise RuntimeError("requires idle developer loader")
    size,bs=dev.info('dsp_a')
    if size!=64*1024**2 or bs!=4096:raise RuntimeError("unexpected dsp_a geometry")
    digest=hashlib.sha256();dest=out/'dsp_a.img'
    with tempfile.NamedTemporaryFile(dir=out,delete=False) as f:
        tmp=Path(f.name)
        try:
            for off in range(0,size,8*1024**2):
                block=dev.read('dsp_a',off,8*1024**2);digest.update(block);f.write(block)
            f.flush();os.fsync(f.fileno());os.replace(tmp,dest)
        finally:tmp.unlink(missing_ok=True)
    img='/build/out/slpi-dsp-firmware/dsp_a.img'
    part=dict(partition='dsp_a',size=size,sha256=digest.hexdigest())
    (out/'identity.json').write_text(json.dumps(part,indent=2)+'\n')
    info=subprocess.run(['file','-b',str(dest)],capture_output=True,text=True,check=True).stdout.strip()
    print('dsp_a',size,'bytes',part['sha256'],info,flush=True)
    if 'ext' not in info.lower():raise RuntimeError('unexpected filesystem; image retained for offline inspection')
    dirs=['/'];seen=set();hits=[]
    while dirs:
        directory=dirs.pop()
        if directory in seen:continue
        if len(seen)>=512: raise RuntimeError("directory count exceeds bound")
        seen.add(directory)
        r=subprocess.run(['docker','exec','oneplus8-build','debugfs','-R','ls -p '+directory,img],capture_output=True,text=True,timeout=30,check=True)
        if 'Filesystem not open' in r.stderr or not r.stdout.strip(): raise RuntimeError("directory read failed")
        for line in r.stdout.splitlines():
            fields=line.split('/')
            if len(fields)<7 or not fields[1].isdigit() or fields[5] in ('.','..',''):continue
            mode=int(fields[2],8)&0o170000;name=fields[5]
            if not re.fullmatch(r'[a-zA-Z0-9._+-]+',name): raise RuntimeError('unexpected firmware entry name')
            path=directory.rstrip('/')+'/'+name
            if mode==0o040000:dirs.append(path)
            elif mode==0o100000 and name in ('oemconfig.so','sns_bring_to_ear.so','wigig_sensing.so'):
                hits.append(dict(path=path,size=fields[6]))
    (out/'matches.json').write_text(json.dumps(dict(directories=len(seen),files=hits),indent=2)+'\n')
    print('scanned',len(seen),'directories; matches',hits)
if __name__=='__main__':main()
