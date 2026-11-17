#!/usr/bin/env python3
"""Build the AWDL adapter in a private copy of the proven cellular/ALS kernel."""
import argparse
from contextlib import contextmanager
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import build_ifnet
from link_objects import VFS_SHA, sha

HERE=Path(__file__).resolve().parent
K=HERE.parents[1]
OUT=K/'out/awdl-kernel'
ALS=K/'tools/nf/cell/als/build.py'
CONTROL_SHA='c5331421b46a8ba2ea284ba45f3b6ea76f8084a97bc3e44c3b5223ed3848dbc0'
BASE_RADIO=K.parent/'oneplus8-audit/cellular-data-next/claude-takeover/ath11k-cell.o'
BASE_RADIO_SHA='a67239fe8edeed537ea0cb5b468c0d2f651b34c88368e4e029b36ac4270f836b'
VFS=K.parent/'oneplus8-audit/stock-cellular-ui/activation-ui/vfs_attrlist.o'
SIM=K/'tools/nf/cell/sim/sim-state.patch'
BASE_RECEIPT=K/'out/cell-settings-sim-kernel.json'
BASE_PIN=K/'out/cell-settings-sim-kernel.txt'
SYMBOLS=('hk_apple80211_ioctl','HKWLANStart','hk_cell_run','hk_cell_quiesce',
         'hk_awdl_provider_register','hk_awdl_provider_set_ready','hk_awdl_provider_unregister',
         'hk_awdl_transport_register','hk_awdl_transport_set_ready','hk_awdl_transport_unregister',
         'hk_awdl_transport_input','hk_awdl_registry_publish','hk_awdl_registry_remove',
         'lx_os_awdl_register','lx_os_awdl_set_ready','lx_os_awdl_unregister','lx_os_awdl_input',
         'lx_os_awdl_peer_present','lx_os_awdl_peer_absent')

def load_als():
    spec=importlib.util.spec_from_file_location('owner_cellular_als',ALS)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def check_file(path, expected):
    if sha(path)!=expected: raise ValueError(f'input content mismatch: {path}')

def verify_base():
    receipt=json.loads(BASE_RECEIPT.read_text())
    pinned=Path(BASE_PIN.read_text().strip())
    if receipt['kernel_sha256']!=CONTROL_SHA or Path(receipt['kernel'])!=pinned:
        raise ValueError('cellular/ALS base pin is not c533')
    for path,digest in [(pinned,CONTROL_SHA),(BASE_RADIO,BASE_RADIO_SHA),(VFS,VFS_SHA),
        (SIM,receipt['sim_state_patch_sha256']),
        (ALS.parent/'registry.patch',receipt['registry_patch_sha256']),
        (ALS.parent/'sensor.patch',receipt['sensor_patch_sha256']),
        (ALS.parent/'display.patch',receipt['display_patch_sha256']),
        (ALS.parent/'HKFrontLightSensor.cpp',receipt['front_light_service_sha256'])]:
        check_file(path,digest)
    return receipt

@contextmanager
def build_environment(radio):
    requested={'CELL_ATH11K_OBJ':str(radio),'CELL_VFS_OBJ':str(VFS)}
    prior={key:os.environ.get(key) for key in requested}
    os.environ.update(requested)
    try: yield
    finally:
        for key,value in prior.items():
            if value is None: os.environ.pop(key,None)
            else: os.environ[key]=value

def replace(text, anchor, updated):
    if text.count(anchor)!=1: raise ValueError('frozen cellular recipe anchor changed: '+anchor)
    return text.replace(anchor,updated)

def private_link(cell,text,work,radio_sha,adapter):
    if adapter and os.environ.get("AWDL_NATIVE_P2P_PROFILE") == "1":
        subprocess.run(["patch","--batch","-p1","-i",str(HERE/"native-p2p-profile.patch")],cwd=cell,check=True)
    directory=cell/'overlay/awdl_link'
    directory.mkdir()
    shutil.copyfile(HERE/'link_objects.py',directory/'hk_awdl_link_plan.py')
    digests=adapter['sha256'] if adapter else {}
    if adapter:
        for name,digest in digests.items():
            shutil.copyfile(Path(adapter['objects'])/name,directory/name)
            check_file(directory/name,digest)
    plan=dict(integrate=bool(adapter),objects=digests,radio_sha256=radio_sha)
    (directory/'plan.json').write_text(json.dumps(plan))
    anchor="    fl = dest / 'link.filelist'"
    text=replace(text,anchor,"    sys.path.insert(0,str(stage/'awdl_link'))\n"
                 "    from hk_awdl_link_plan import apply\n"
                 "    apply(paths,obj,root)\n"+anchor)
    anchor="            run('docker', 'cp', CONTAINER + ':' + w + '/candidate.mach', str(tmp / 'candidate.mach'))"
    text=replace(text,anchor,anchor+"\n            run('docker', 'cp', CONTAINER + ':' + w + '/awdl-link-receipt.json', "+repr(str(work/'link.json'))+")")
    old_log="KERNEL / 'out' / ('cell-check.log' if check else 'cell-als-settings-build.log')"
    text=replace(text,old_log,"Path("+repr(str(work/'link.log'))+")")
    old_dest="KERNEL / 'out/kernels' / ('cell-' + k + '.mach')"
    return replace(text,old_dest,"Path("+repr(str(work))+") / ('cell-' + k + '.mach')")

def relink(als,work,radio,radio_sha,adapter):
    work.mkdir()
    pin=work/'result.txt'
    try:
        with build_environment(radio):
            als.main(['--settings','--sim-state-patch',str(SIM),'--output',str(pin)],
                     extend_private_link=lambda cell,text:private_link(cell,text,work,radio_sha,adapter))
    except (Exception, SystemExit):
        log=work/'link.log'
        if log.exists(): print(log.read_text(errors='replace')[-6000:],file=sys.stderr)
        raise
    kernel=Path(pin.read_text().strip())
    receipt=json.loads(pin.with_suffix('.json').read_text())
    check_file(kernel,receipt['kernel_sha256'])
    return kernel,receipt,json.loads((work/'link.json').read_text())

def symbols(path,flag):
    result={}
    for line in subprocess.check_output(['nm',flag,str(path)],text=True).splitlines():
        fields=line.split()
        if len(fields)<2: continue
        result.setdefault(fields[-1],[]).append(fields[-2])
    return result

def atomic_text(path,data):
    fd,tmp=tempfile.mkstemp(prefix='.'+path.name+'-',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(tmp,path)
    finally:
        Path(tmp).unlink(missing_ok=True)

def store_result(output,kernel,receipt):
    dest=output.parent/(output.stem+'.artifacts')
    dest.mkdir(mode=0o700,exist_ok=True)
    target=dest/('cell-'+receipt['kernel_sha256']+'.mach')
    if not target.exists():
        with tempfile.NamedTemporaryFile(dir=dest,prefix='.kernel-',delete=False) as stream:
            tmp=Path(stream.name)
        try:
            shutil.copyfile(kernel,tmp)
            check_file(tmp,receipt['kernel_sha256'])
            tmp.chmod(0o400)
            os.replace(tmp,target)
        finally:
            tmp.unlink(missing_ok=True)
    check_file(target,receipt['kernel_sha256'])
    receipt['kernel']=str(target)
    metadata=json.dumps(receipt,indent=2)+'\n'
    if not target.with_suffix('.json').exists():
        atomic_text(target.with_suffix('.json'),metadata)
    atomic_text(output.with_suffix('.json'),metadata)
    atomic_text(output,str(target)+'\n')

def build(output,selected,expected):
    als=load_als()
    base=verify_base()
    selected=selected.resolve()
    radio_sha=sha(selected)
    if expected and radio_sha!=expected: raise ValueError('radio does not match --radio-sha')
    local_files=[HERE/name for name in ('build_kernel.py','build_ifnet.py','build_control.py','link_objects.py',
        'apple80211-control.patch','apple80211_control.inc','apple80211_provider.h','ifnet-registry.patch',
        'async_events.inc','bonjour_store.inc','bonjour_offload.h','native_query.h',
        'ifnet_registry.inc','ifnet_transport.h','lx_awdl_transport.h','ifnet_adapter.c','ifnet_peer.inc')]
    # Record the same input view before and after the concurrent build.
    if os.environ.get('AWDL_NATIVE_P2P_PROFILE') == '1':
        local_files.append(HERE/'native-p2p-profile.patch')
    if os.environ.get('AWDL_NATIVE_ADDRESS') == '1':
        local_files.append(HERE/'native_address.inc')
    sources={str(path.relative_to(K)):sha(path) for path in local_files}
    OUT.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='private-link-',dir=OUT) as temp:
        work=Path(temp)
        control_kernel,control,control_plan=relink(als,work/'control',BASE_RADIO,BASE_RADIO_SHA,None)
        check_file(control_kernel,CONTROL_SHA)
        for key in ('base_commit','cell_driver_sha256','settings_service','registry_patch_sha256','display_patch_sha256','sensor_patch_sha256'):
            if control[key]!=base[key]:raise ValueError('control input differs: '+key)
        radio=work/'selected-ath11k.o'
        shutil.copyfile(selected,radio);check_file(radio,radio_sha)
        adapter=build_ifnet.main(work/'adapter')
        kernel,src,plan=relink(als,work/'candidate',radio,radio_sha,adapter)
        syms=symbols(kernel,'-g')
        for name in SYMBOLS:
            definitions=[t for t in syms.get('_'+name,[]) if t!='U']
            if len(definitions)!=1:raise ValueError('expected one defined symbol in final kernel: '+name)
        imports=sorted(s for s in symbols(radio,'-u') if s.startswith(('_lx_os_awdl_','_hk_awdl_transport_')))
        for path in local_files:check_file(path,sources[str(path.relative_to(K))])
        result=dict(kind='private-awdl-kernel',kernel_sha256=src['kernel_sha256'],kernel_bytes=kernel.stat().st_size,
            control_sha256=control['kernel_sha256'],control_radio_sha256=BASE_RADIO_SHA,
            base_receipt_sha256=sha(BASE_RECEIPT),als_builder_sha256=sha(ALS),
            radio=str(selected),radio_sha256=radio_sha,radio_awdl_imports=imports,
            control_link=control_plan,link=plan,objects=adapter['sha256'],staged_sources=adapter['sources'],
            local_sources=sources,verified_kernel_symbols=list(SYMBOLS),boot_registration_added=False,device_verified=False)
        store_result(output,kernel,result)
        print('control',CONTROL_SHA,'candidate',result['kernel_sha256'],'pin',output,flush=True)

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True,help='private .txt kernel pin; a .json receipt is written beside it')
    p.add_argument('--radio',type=Path,help='ath11k Mach-O; defaults to CELL_ATH11K_OBJ or the c533 radio')
    p.add_argument('--radio-sha',help='reject a different radio object')
    args=p.parse_args();output=args.output.resolve()
    protected={K/'out'/n for n in ('cell-kernel.txt','cell-kernel-installed.txt','cell-als-kernel.txt',
        'cell-als-settings-kernel.txt','cell-settings-sim-kernel.txt','cell-sim-kernel.txt')}
    if output.suffix!='.txt' or output in protected or output==OUT/'kernel.txt':
        p.error('--output must be a private .txt pin independent of known feature pins')
    selected=args.radio or Path(os.environ.get('CELL_ATH11K_OBJ') or BASE_RADIO)
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.with_suffix('.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        receipt=output.with_suffix('.json')
        if output.exists() or receipt.exists():
            if not receipt.is_file() or json.loads(receipt.read_text()).get('kind')!='private-awdl-kernel':
                p.error('requested output belongs to a different build')
        build(output,selected,args.radio_sha)

if __name__=='__main__':
    main()
