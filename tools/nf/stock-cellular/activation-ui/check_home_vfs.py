"""Recompile the stock-preserving Home view from its original retained source and flags."""
import hashlib,json,subprocess,tempfile
from pathlib import Path
H=Path(__file__).resolve().parent;K=H.parents[3];A=K.parent/'oneplus8-audit'
OUT=A/'stock-cellular-ui/activation-ui';VIEW=A/'gnss-settings-next/bulk-union/home-view'
expected=json.loads((VIEW/'wifi/kernel.json').read_text())['home_vfs_object_sha256']
source_sha=hashlib.sha256((VIEW/'vfs_attrlist.c').read_bytes()).hexdigest()
base='/tmp/gnss-union-bulk-OX4ljJPy'
script=r'''
. /build/xnu-env.sh
python3 - "$1" "$2" "$3" <<'INNER'
import pathlib,shlex,subprocess,hashlib,sys
p=pathlib.Path(sys.argv[1]);t=pathlib.Path(sys.argv[2]);src=p/'vfs_attrlist.c';assert hashlib.sha256(src.read_bytes()).hexdigest()==sys.argv[3]
base=pathlib.Path('/build/xnu-src/nf-tree/xnu-8796.121.2');bsd=base/'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT'
raw=(bsd/'.CFLAGS').read_bytes().strip(b'\0\r\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)')
subprocess.run(shlex.split(raw)+['-I',str(base/'bsd/vfs'),'-c',str(src),'-o',str(t/'vfs_attrlist.o')],cwd=bsd,check=True)
INNER
'''
c='oneplus8-build';r=subprocess.check_output(['docker','exec',c,'mktemp','-d','/tmp/cellular-vfs-XXXXXXXX'],text=True).strip()
assert r.startswith('/tmp/cellular-vfs-')
try:
 subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-i',c,'bash','-se','--',base,r,source_sha],input=script,text=True,check=True)
 with tempfile.TemporaryDirectory()as d:
  obj=Path(d)/'vfs_attrlist.o';subprocess.run(['docker','cp',c+':'+r+'/'+obj.name,str(obj)],check=True)
  actual=hashlib.sha256(obj.read_bytes()).hexdigest();assert actual==expected,(expected,actual)
  if not (OUT/obj.name).exists():(OUT/obj.name).write_bytes(obj.read_bytes())
  assert hashlib.sha256((OUT/obj.name).read_bytes()).hexdigest()==expected
 receipt=dict(source_sha256=source_sha,object_sha256=actual,byte_identical_to_ticket_258=True,boot_option='home_apps=1')
 (OUT/'vfs.json').write_text(json.dumps(receipt,indent=2)+'\n');print(actual)
finally:subprocess.run(['docker','exec',c,'rm','-rf','--',r],check=True)
