"""Add one bounded network/debug CLI to the checked SSH RAM root."""
import json,os,subprocess,sys,tempfile,shutil
from pathlib import Path
K=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(K/'tools/nf/cell/als'));from settings_root import contents,sha,apfs
sys.path.insert(0,str(K/'tools/airdrop'));from native_root import budget
BASE_SHA='548342710a7cfddc13d7b0989dc594f1942b589f2c5a3dc55a2f710154e7476a'
def root_meta(path):
 c=apfs.Container(str(path))
 try:
  v=apfs.pick_volume(c,None);i=v.inode(apfs.ROOT_DIR_INO_NUM);return(i.mode,i.owner,i.group)
 finally:c.close()
def main():
 os.umask(0o077)
 base=K/'out/ssh-debug/ssh-connect.apfs';probe=K/'out/airdrop-ui/network-probe/airdrop_network_probe'
 kernel=Path((K/'out/awdl-kernel/integration/wmi-amsdu49.txt').read_text().strip())
 if sha(base)!=BASE_SHA:raise ValueError('base root changed')
 original_root=root_meta(base)
 if (original_root[0]&0o777)!=0o755:raise ValueError('unexpected base root permissions')
 out=K/'out/airdrop-ui/network-probe';name='bin/airdrop_network_probe'
 with tempfile.TemporaryDirectory(dir=out,prefix='root-') as tmp:
  stage=Path(tmp)/'root';stage.mkdir();before=contents(base,stage)
  if name in before:raise ValueError('probe already present')
  shutil.copyfile(probe,stage/name);(stage/name).chmod(0o755);stage.chmod(0o755)
  image=Path(tmp)/'network-probe.apfs'
  subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(stage)],check=True,stdout=subprocess.DEVNULL)
  after=contents(image)
  if set(after)!=set(before)|{name}:raise ValueError('unexpected file set')
  for n,v in before.items():
   if after[n]!=v:raise ValueError('existing entry changed: '+n)
  if root_meta(image)!=original_root:raise ValueError('root inode metadata changed')
  if (after[name][0]&0o777)!=0o755 or after[name][1:3]!=(0,0) or after[name][-1]!=sha(probe):raise ValueError('probe metadata mismatch')
  checks=budget(image,kernel,sha(kernel));h=sha(image);target=out/('network-root-'+h+'.apfs');image.chmod(0o400);image.replace(target)
  r=dict(root=str(target),root_sha256=h,base_sha256=BASE_SHA,unchanged_entries=len(before),added_paths=[name],root_metadata_preserved=True,probe_sha256=sha(probe),**checks)
  (out/'root.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
if __name__=='__main__':main()
