"""Private kernel, signed loopback helper and verified RAM overlay. No device operations."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
from build_loader import ROOT, HERE, OUTPUT, need
from prepare_kernel import prepare
sys.path.insert(0,str(ROOT.parent/'oneplus8-audit/metal-bringup-next'))
from prepare_ab import sha,verify


def main():
    os.umask(0o077)
    OUTPUT.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUTPUT) as t:
        work=Path(t)
        private=work/'src';private.mkdir();prepare(private)
        for name in ('build_private.sh','portable_http.c'):shutil.copyfile(HERE/name,private/name)
        container='oneplus8-build'
        temp=subprocess.check_output(['docker','exec',container,'mktemp','-d','/tmp/portable-XXXXXXXX'],text=True).strip()
        try:
            subprocess.run(['docker','cp',str(private)+'/.',container+':'+temp],check=True)
            subprocess.run(['docker','exec',container,'bash',temp+'/build_private.sh',temp],check=True)
            for name in ('portable.mach','portable_http'):
                subprocess.run(['docker','cp',container+':'+temp+'/'+name,str(work/name)],check=True)
        finally:
            subprocess.run(['docker','exec',container,'rm','-rf','--',temp],check=True)
        subprocess.run(['codesign','--force','--sign','-','--identifier','com.hackios.portable-http',
                        '--timestamp=none',str(work/'portable_http')],check=True)
        subprocess.run(['codesign','--verify','--strict',str(work/'portable_http')],check=True)
        tree=work/'tree';shutil.copytree(ROOT/'out/radio-state-root',tree,symlinks=True);tree.chmod(0o755)
        for link in tree.rglob('*'):
            if link.is_symlink():os.chmod(link,0o777,follow_symlinks=False)
        need(not (tree/'Applications').exists(),'unexpected Applications overlay')
        shutil.copyfile(work/'portable_http',tree/'bin/portable_http');(tree/'bin/portable_http').chmod(0o755)
        jobs='/hackios/bin/portable_http\n/hackios/bin/rwtouch uptime 80\n/hackios/bin/battery_test open http://127.0.0.1:8765/\n'
        (tree/'etc/rc.jobs').write_text(jobs)
        subprocess.run([sys.executable,ROOT/'tools/apfs/mkapfs.py','-z','--plain','fw','-o',work/'portable.apfs','hackios='+str(tree)],
                       check=True,stdout=subprocess.DEVNULL)
        checked=verify(work/'portable.apfs',tree)
        pins={}
        for name in ('portable.mach','portable.apfs'):
            path=work/name;digest=sha(path);dest=OUTPUT/(path.stem+'-'+digest+path.suffix)
            path.replace(dest);dest.chmod(0o400);pins[path.suffix[1:]]=digest
        pins.update(resident=sha(ROOT/'userland/flashboot.apfs'),helper=sha(tree/'bin/portable_http'),verified=checked)
        (OUTPUT/'profile.json').write_text(json.dumps(pins,indent=2)+'\n')
        print(json.dumps(pins))


if __name__=='__main__':main()
