"""Scope the local Bonjour grouping bridge to sharingd in a RAM-only root."""
import argparse,hashlib,json,os,plistlib,shutil,subprocess,sys,tempfile
from pathlib import Path
K=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(K/'tools/nf/cell/als'));from settings_root import contents,sha,apfs
sys.path.insert(0,str(K/'tools/airdrop'));from native_root import budget
BASE_SHA='a872a97b5a96686e19cdf04aa9f8d71e0d8c71991247df2d977df29aff14cec3'
BIN_SHA='bcf494f997e718fabcb166cd049aea5cd4dfe297d231a95657c7e3d715912cc2'
def rootmeta(path):
 c=apfs.Container(str(path))
 try:
  v=apfs.pick_volume(c,None);i=v.inode(apfs.ROOT_DIR_INO_NUM);return i.mode,i.owner,i.group
 finally:c.close()
def main():
 options=argparse.ArgumentParser();options.add_argument('--observe-only',action='store_true');options.add_argument('--list-unknown',action='store_true');args=options.parse_args()
 os.umask(0o077);out=K/'out/airdrop-ui/node-key';base=K/'out/airdrop-ui/network-probe'/('network-root-'+BASE_SHA+'.apfs')
 lib=out/'libbonjour_node_key.dylib';probe=out/'node_images';kernel=Path((K/'out/awdl-kernel/integration/wmi-mdns-carrier49.txt').read_text().strip())
 if sha(base)!=BASE_SHA or sha(K/'ios165/rootfs/usr/libexec/sharingd')!=BIN_SHA:raise ValueError('base or sharingd identity changed')
 config='etc/inject.conf';job='LaunchDaemons/com.apple.sharingd.plist';name='lib/libbonjour_node_key.dylib';probe_name='bin/node_images'
 with tempfile.TemporaryDirectory(dir=out,prefix='root-') as tmp:
  stage=Path(tmp)/'root';stage.mkdir();before=contents(base,stage)
  if name in before or 'lib' not in before:raise ValueError('unexpected library layout')
  p=stage/job;old=p.read_bytes();state=plistlib.loads(old)
  if state.get('Label')!='com.apple.sharingd':raise ValueError('unexpected daemon')
  env=dict(state.get('EnvironmentVariables',{}))
  env['HK_AIRDROP_BONJOUR_LOCAL_KEY']='1'
  if args.observe_only:env['HK_AIRDROP_OBSERVE_ONLY']='1'
  if args.list_unknown:
   if not args.observe_only:raise ValueError('UI inclusion requires original grouping preserved')
   env['HK_AIRDROP_LIST_UNKNOWN']='1'
  state['EnvironmentVariables']=env
  inject_old=(stage/config).read_bytes();inject_new=inject_old+(b'' if inject_old.endswith(b'\n') else b'\n')+('/hackios/'+name+' sharingd\n').encode()
  if len(inject_new)>=512:raise ValueError('kernel injection configuration capacity')
  (stage/config).write_bytes(inject_new)
  new=plistlib.dumps(state,fmt=plistlib.FMT_BINARY if old.startswith(b'bplist00') else plistlib.FMT_XML,sort_keys=False);p.write_bytes(new)
  shutil.copyfile(lib,stage/name);(stage/name).chmod(0o755);shutil.copyfile(probe,stage/probe_name);(stage/probe_name).chmod(0o755);stage.chmod(0o755)
  image=Path(tmp)/'root.apfs'
  subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(stage)],check=True,stdout=subprocess.DEVNULL)
  after=contents(image)
  if set(after)!=set(before)|{name,probe_name}:raise ValueError('unexpected file set')
  for n,record in before.items():
   wanted=record[:3]+(hashlib.sha256(new if n==job else inject_new).hexdigest(),) if n in (job,config) else record
   if after[n]!=wanted:raise ValueError('unexpected existing entry change: '+n)
  if rootmeta(image)!=rootmeta(base):raise ValueError('root metadata changed')
  if (after[name][0]&0o777)!=0o755 or after[name][1:3]!=(0,0) or after[name][-1]!=sha(lib):raise ValueError('library metadata mismatch')
  if (after[probe_name][0]&0o777)!=0o755 or after[probe_name][1:3]!=(0,0) or after[probe_name][-1]!=sha(probe):raise ValueError('probe metadata mismatch')
  checks=budget(image,kernel,sha(kernel));h=sha(image);target=out/('node-key-root-'+h+'.apfs');image.chmod(0o400);image.replace(target)
  r=dict(root=str(target),root_sha256=h,base_sha256=BASE_SHA,added_paths=[name,probe_name],modified_paths=[job,config],unchanged_entries=len(before)-2,root_metadata_preserved=True,sharingd_sha256=BIN_SHA,library_sha256=sha(lib),scope='native UI lists real unknown AirDrop models; original identity/grouping untouched' if args.list_unknown else 'sharingd observer only; original grouping untouched' if args.observe_only else 'sharingd anonymous Bonjour local grouping only',**checks)
  (out/'root.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
if __name__=='__main__':main()
