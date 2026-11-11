"""Compile the normal four BSD units with NF flags and link them into an isolated XNU.
Uses the ordinary configured link inputs. No #269/#270 diagnostic or new boot argument.
"""
from pathlib import Path
import hashlib,json,subprocess,sys
H=Path(__file__).resolve().parent;K=H.parents[1]
assert sys.argv[1:] in ([],['--with-frontend'])
front=bool(sys.argv[1:])
A=K.parent/'oneplus8-audit/stock-cellular-ui'/('maintained-frontend' if front else 'maintained-backend');A.mkdir(parents=True,exist_ok=True)
units=('hk_qrtr_core','hk_qrtr_mhi','hk_abp_nas','hk_abp_nas_worker')+(('hk_qmux_frontend',) if front else ())
for tree in (K/'xnu-src/xnu-8796.121.2',K/'xnu-src/nf-tree/xnu-8796.121.2'):
    conf=(tree/'bsd/conf/files').read_text().splitlines()
    for unit in units:
        name='bsd/dev/'+unit+'.c'
        assert sum(line.split()==[name,'standard'] for line in conf)==1
        assert (tree/name).read_bytes()==(K/'xnu/files'/name).read_bytes()
    if front:
        name='iokit/Kernel/HKCellularQMUX.cpp'
        assert sum(v.split()==[name,'optional','iokitcpp'] for v in (tree/'iokit/conf/files').read_text().splitlines())==1
        assert (tree/name).read_bytes()==(K/'xnu/files'/name).read_bytes()

c='oneplus8-build';tmp=subprocess.check_output(['docker','exec',c,'mktemp','-d','/tmp/nf-nas-backend-XXXXXXXX'],text=True).strip()
assert tmp.startswith('/tmp/nf-nas-backend-')
script=r'''
. /build/xnu-env.sh
python3 - "$1" "$2" <<'INNER'
from pathlib import Path
import shlex,subprocess,sys,json,hashlib
tmp=Path(sys.argv[1]);base=Path('/build/xnu-src/nf-tree/xnu-8796.121.2');obj=base/'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837';bsd=obj/'bsd/DEVELOPMENT'
def flags(p):return shlex.split(p.read_bytes().strip(b'\0\r\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)'))
cc=flags(bsd/'.CFLAGS');ld=flags(obj/'.LDFLAGS');assert ld.pop()=='-lcc_kext'
files=(obj/'link.filelist').read_text().splitlines();hashes={}
frontend=sys.argv[2]=='front'
for name in ('hk_qrtr_core','hk_qrtr_mhi','hk_abp_nas','hk_abp_nas_worker')+(('hk_qmux_frontend',) if frontend else ()):
    src=base/('bsd/dev/'+name+'.c');out=tmp/(name+'.o')
    subprocess.run(cc+['-I',str(base/'bsd'),'-c',str(src),'-o',str(out)],cwd=bsd,check=True)
    hashes[name]=hashlib.sha256(out.read_bytes()).hexdigest()
    old=str(bsd/out.name)
    if old in files:assert files.count(old)==1;files[files.index(old)]=str(out)
    else:files.append(str(out))
if frontend:
    io=obj/'iokit/DEVELOPMENT'
    cc=flags(io/'.CXXFLAGS')+flags(io/'.CFLAGS')[1:]
    out=tmp/'HKCellularQMUX.o'
    subprocess.run(cc+['-I','/build/xnu/files/bsd','-c',str(base/'iokit/Kernel/HKCellularQMUX.cpp'),'-o',str(out)],cwd=io,check=True)
    hashes['HKCellularQMUX']=hashlib.sha256(out.read_bytes()).hexdigest()
    old=str(io/out.name)
    if old in files: assert files.count(old)==1; files[files.index(old)]=str(out)
    else: files.append(str(out))
listing=tmp/'files';listing.write_text('\n'.join(files)+'\n')
binary=tmp/'kernel.unstripped'
subprocess.run(ld+['-filelist',str(listing),'version.o','lastkerneldataconst.o','lastkernelconstructor.o','nonlto.o','-o',str(binary),'-lcc_kext','-lfirehose_kernel','-lpthread_kext','/build/xnu-deps/build/ath11k/out/ath11k.o'],cwd=obj,check=True)
subprocess.run(['strip','-S',str(binary),'-o',str(tmp/'kernel.mach')],check=True)
symbols=subprocess.check_output(['llvm-nm',str(tmp/'kernel.mach')],text=True)
names=('hk_qrtr_mhi_nas_open','hk_abp_nas_attach','hk_abp_worker_arm','hk_abp_worker_rearm_after_disconnect','hk_abp_worker_generation','hk_abp_worker_ready','hk_abp_worker_rx','hk_abp_worker_poll','hk_abp_worker_open','hk_abp_worker_write','hk_abp_worker_close','hk_abp_worker_stop','hk_abp_worker_finish','hk_abp_worker_drained','hk_abp_worker_timeout','hk_abp_worker_never_started')
if frontend:
    names+=('hk_qmux_frontend_poll','hk_qmux_frontend_stop','hk_qmux_frontend_request','hk_qmux_frontend_take','hk_qmux_frontend_cancel','hk_qmux_registry_publish','hk_qmux_registry_withdraw')
    assert 'HKCellularQMUXChannel' in symbols
assert all(any(l.endswith(' _'+n) for l in symbols.splitlines()) for n in names)
assert all(not any(l.endswith(' _'+n) for l in symbols.splitlines()) for n in ('qn_tick','qno_tick','qn_emit','qno_emit'))
(tmp/'receipt.json').write_text(json.dumps({'kernel_sha256':hashlib.sha256((tmp/'kernel.mach').read_bytes()).hexdigest(),'objects':hashes,'symbols':names,'hardware_tested':False,'diagnostic_requests_on_boot':0},indent=2)+'\n')
INNER
'''
try:
    with (A/'build.log').open('w') as log:
        subprocess.run(['docker','exec','-i','-e','THEOS=/build/local-theos',c,'bash','-se','--',tmp,'front' if front else 'base'],input=script,text=True,stdout=log,stderr=subprocess.STDOUT,check=True)
    for name in ('kernel.mach','receipt.json'):subprocess.run(['docker','cp',c+':'+tmp+'/'+name,str(A/name)],check=True)
    receipt=json.loads((A/'receipt.json').read_text());assert hashlib.sha256((A/'kernel.mach').read_bytes()).hexdigest()==receipt['kernel_sha256']
    print(receipt['kernel_sha256'],len(receipt['symbols']),'linked symbols, driver remains inactive until an explicit caller')
finally:subprocess.run(['docker','exec',c,'rm','-rf','--',tmp],check=True)
