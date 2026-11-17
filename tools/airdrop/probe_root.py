"""Add one checked read-only tool to the frozen working RAM root; no jobs changed."""
import argparse,json,os,shutil,subprocess,sys,tempfile
from pathlib import Path
from native_root import K,budget
sys.path.insert(0,str(K/'tools/nf/cell/als'))
from settings_root import contents,sha
BASE='e23f0e5e09e1899fdc4866c2b21c986c7c0277eccf6aa66e6364b10db79ef46d'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True);p.add_argument('--probe-sha',required=True)
    p.add_argument('--kernel',type=Path,required=True);p.add_argument('--kernel-sha',required=True)
    a=p.parse_args();os.umask(0o077)
    base=K/'out/airdrop-ui/awake-share'/('native-root-'+BASE+'.apfs')
    if sha(base)!=BASE or sha(a.probe)!=a.probe_sha:raise ValueError('frozen inputs changed')
    out=K/'out/airdrop-ui/stack-observe';out.mkdir(parents=True,exist_ok=True)
    name='bin/airdrop_stack_probe'
    with tempfile.TemporaryDirectory(dir=out,prefix='probe-root-') as tmp:
        stage=Path(tmp)/'root';stage.mkdir();before=contents(base,stage)
        if name in before:raise ValueError('probe already present')
        shutil.copyfile(a.probe,stage/name);(stage/name).chmod(0o755);stage.chmod(0o755)
        image=Path(tmp)/'image.apfs'
        subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(stage)],check=True,stdout=subprocess.DEVNULL)
        after=contents(image)
        if set(after)!=set(before)|{name}:raise ValueError('file set changed')
        for n,v in before.items():
            if after[n]!=v:raise ValueError('original file changed: '+n)
        if (after[name][0]&0o777)!=0o755 or after[name][1:3]!=(0,0) or after[name][-1]!=a.probe_sha:
            raise ValueError('probe readback mismatch')
        checks=budget(image,a.kernel,a.kernel_sha)
        digest=sha(image);target=out/('probe-root-'+digest+'.apfs');image.chmod(0o400);image.replace(target)
        r=dict(root=str(target),root_sha256=digest,base_sha256=BASE,probe_sha256=a.probe_sha,
            unchanged_entries=len(before),added_paths=[name],modified_existing=[],**checks)
        (out/'root.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
if __name__=='__main__':main()
