"""Offline private relink, first requiring the exact stock UI ticket #268 / #267 control."""
import hashlib,importlib.util,json,shutil,subprocess,tempfile
from pathlib import Path
H=Path(__file__).resolve().parent;K=H.parents[1];A=K.parent/'oneplus8-audit'
D=A/'stock-cellular-ui/qmux-nas';D.mkdir(exist_ok=True)
orig=A/'cellular-data-next/uim-endpoints/prepare.py'
spec=importlib.util.spec_from_file_location('uim_source',orig);uim=importlib.util.module_from_spec(spec);spec.loader.exec_module(uim)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def replace(s,a,b):
    assert s.count(a)==1,a
    return s.replace(a,b)
files,_=uim.variants()
with tempfile.TemporaryDirectory() as folder:
    t=Path(folder)
    for label in ('reference','candidate'):
        for rel,(_base,src) in files.items():
            if label=='candidate' and rel.endswith('modem_boot.inc'):
                at='static int\nmdm_radio_init(void)'
                src=replace(src,at,(H/'phone_nas.inc').read_text()+'\n'+at)
                src=replace(src,'\t\tmdm_radio_query(tenths);','\t\tmdm_radio_query(tenths);\n\t\tqn_tick(tenths);')
                src=replace(src,'\tuep_close();','\tqn_close("modem_teardown");\n\tuep_close();')
            p=t/label/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(src)
        if label=='candidate':
            for rel in ('bsd/dev/hk_qrtr_mhi.c','bsd/dev/hk_qmux_nas.inc','bsd/sys/hk_qmux_nas.h'):
                src=K/'xnu/files'/rel;dest=t/label/'xnu/files'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
    link=(A/'cellular-data-next/wds-observer/link.py').read_text()
    link=replace(link,"['ref233','control263','maintained','wds263']","['reference','candidate']")
    a="for unit in [inc/'instantnoodle_modem.c', fs/'dev/hk_qmi_radio.c']:"
    link=replace(link,a,"for unit in [inc/'instantnoodle_modem.c',fs/'dev/hk_qmi_radio.c']+([fs/'dev/hk_qrtr_mhi.c'] if label=='candidate' else []):")
    (t/'link.py').write_text(link)
    c='oneplus8-build';remote=subprocess.check_output(['docker','exec',c,'mktemp','-d','/tmp/nas-qmux-XXXXXXXX'],text=True).strip();assert remote.startswith('/tmp/nas-qmux-')
    try:
        subprocess.run(['docker','cp',str(t)+'/.',c+':'+remote],check=True)
        with (D/'build.log').open('w') as log:
            subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-e','W='+remote,c,'bash','-ec','. /build/xnu-env.sh; python3 "$W/link.py" "$W"'],stdout=log,stderr=subprocess.STDOUT,check=True)
        for label in ('reference','candidate'):
            subprocess.run(['docker','cp',c+':'+remote+'/'+label+'.mach',str(t/(label+'.mach'))],check=True)
        ref=digest(t/'reference.mach');assert ref==json.loads((orig.parent/'pin.json').read_text())['kernel']
        sha=digest(t/'candidate.mach');dst=K/'out/kernels'/f'nf-qmux-nas-{sha}.mach'
        if not dst.exists():shutil.copyfile(t/'candidate.mach',dst);dst.chmod(0o444)
        assert sha==digest(dst)
        receipt=dict(kernel=str(dst),kernel_sha256=sha,reference_kernel_sha256=ref,control_byte_identical=True,
                     control_ticket=267,ui_control_ticket=268,profile='ui_qmux=1',max_firmware_reads=2,selector='NAS 0x0024')
        (D/'kernel.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
    finally:subprocess.run(['docker','exec',c,'rm','-rf','--',remote],check=True)
