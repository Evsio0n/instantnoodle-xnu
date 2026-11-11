#!/usr/bin/env python3
"""build.py [--check]: the kernel with the cellular path (boot-arg cell=1). The sources of the last kernel tested before the port
(base/: 45 files as they were pinned then; the tree's own copies have moved on since), overlay/ on top of them, and the driver itself
(this directory's *.c, *.h and cell_dt.cpp), compiled and relinked in the oneplus8-build container against the objects of the
container's nf-tree build (BUILD_NF). --check compiles the new units only. No device I/O. Writes out/kernels/cell-SHA256.mach and
names it in out/cell-kernel.txt.

Two objects are not made here and are not in git: vfs_attrlist.o (bsd/vfs/vfs_attrlist.c with the tree's patch, as
tools/nf/stock-cellular/activation-ui/check_home_vfs.py builds it; CELL_VFS_OBJ) and the Wi-Fi driver's object built against this
kernel's sys/hk_qmi.h (tools/mk-ath11k.sh; CELL_ATH11K_OBJ). Their defaults are where they lie beside this checkout."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
INNER = sys.argv[1:2] == ['inner']              # (in the container the paths below do not exist)
KERNEL = None if INNER else HERE.parents[2]
BASE = None if INNER else HERE / 'base'
SIDE = None if INNER else KERNEL.parent / 'oneplus8-audit'
VFS = None if INNER else Path(os.environ.get('CELL_VFS_OBJ') or SIDE / 'stock-cellular-ui/activation-ui/vfs_attrlist.o')    # the object the tested kernels were linked with
ATH = None if INNER else Path(os.environ.get('CELL_ATH11K_OBJ') or SIDE / 'cellular-data-next/claude-takeover/ath11k-cell.o')
DRIVER = ('instantnoodle_cell.c', 'cell_net.c', 'cell_net.h', 'hk_mhi.c', 'hk_mhi.h', 'cell_dt.cpp')
CONTAINER = 'oneplus8-build'
UNITS = ['hk_qrtr_core', 'hk_qrtr_mhi', 'hk_abp_nas', 'hk_abp_nas_worker', 'hk_qmi_radio', 'modem_mhi', 'instantnoodle_modem',
         'hk_qmux_frontend', 'HKCellularQMUX', 'hk_qmi', 'hk_wifi', 'kern_minimac', 'subr_log_stream',
         'hk_adsp', 'hk_sound', 'hk_mic', 'hk_speaker', 'instantnoodle_speaker']       # (the last five: the tree's audio drivers with a call's sound, voice_port.py)       # (hk_wifi: the tree's own, plus the panic text at its halt: overlay)
NEW = ['instantnoodle_cell', 'hk_mhi', 'cell_net', 'cell_dt']
CXX = {'HKCellularQMUX': 'xnu/files/iokit/Kernel/HKCellularQMUX.cpp', 'cell_dt': 'tools/nf/cell/cell_dt.cpp'}     # compiled as IOKit's C++          # tools/nf/cell: appended to the link
MAY_APPEND = {'hk_abp_nas', 'hk_abp_nas_worker', 'hk_qmux_frontend', 'HKCellularQMUX'} | set(NEW)


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, **kw)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def flags(path):
    return shlex.split(path.read_bytes().strip(b'\0\r\n ').decode().replace('((1024 * 256) / MCLBYTES)', '((1024*256)/MCLBYTES)'))


def inner(root, check):
    nf = Path('/build/xnu-src/nf-tree/xnu-8796.121.2')
    obj = nf / 'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837'
    bsd, io = obj / 'bsd/DEVELOPMENT', obj / 'iokit/DEVELOPMENT'
    stage, dest = root / 'stage', root / 'candidate'
    dest.mkdir()
    paths = (obj / 'link.filelist').read_text().splitlines()
    fs = stage / 'xnu/files/bsd'
    for name in (NEW if check else UNITS + NEW):
        if name in CXX:
            wd, cc = io, flags(io / '.CXXFLAGS')
            cc[1:1] = ['-I', str(fs), '-I', str(obj / 'bsd')]
            cc += flags(io / '.CFLAGS')[1:]
            src = stage / CXX[name]
        else:
            wd, cc = bsd, flags(bsd / '.CFLAGS')
            cc[1:1] = ['-I', str(fs), '-I', str(nf / 'bsd/dev')]
            # (kern_minimac: bsd/kern's sandbox stand-in, the tree's own plus the answer the BlastDoor services need: overlay)
            # (subr_log_stream: xnu's own with one line put right, the free of the log stream's block map: its close ended the system, run 013832)
            where = stage / 'tools/nf/cell' if name in NEW else stage / 'tools/nf/modem' if name in ('modem_mhi', 'instantnoodle_modem') else fs / 'kern' if name in ('kern_minimac', 'subr_log_stream') else fs / 'dev'
            src = where / (name + '.c')
        unit = dest / (name + '.o')
        run(*cc, '-c', str(src), '-o', str(unit), cwd=wd)
        old = str(wd / (name + '.o'))
        if old in paths:
            assert paths.count(old) == 1, old
            paths[paths.index(old)] = str(unit)
        else:
            assert name in MAY_APPEND, old
            paths.append(str(unit))
    if check:
        print('the new units compile')
        return
    prior = str(bsd / 'vfs_attrlist.o')
    assert paths.count(prior) == 1
    paths[paths.index(prior)] = str(root / 'vfs_attrlist.o')
    fl = dest / 'link.filelist'
    fl.write_text('\n'.join(paths) + '\n')
    ld = flags(obj / '.LDFLAGS')
    assert ld.pop() == '-lcc_kext'
    debug = dest / 'kernel.unstripped'
    run(*ld, '-filelist', str(fl), 'version.o', 'lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o', '-o', str(debug),
        '-lcc_kext', '-lfirehose_kernel', '-lpthread_kext', str(root / 'ath11k-cell.o'), cwd=obj)
    run('strip', '-S', str(debug), '-o', str(root / 'candidate.mach'))
    symbols = run('llvm-nm', str(debug), capture_output=True).stdout
    for want in ('_hk_cell_run', '_hk_cell_quiesce', '_hk_mhi_init', '_hk_modem_quiesce', '_hk_cell_dt_baseband', '_hk_cell_net_attach'):
        assert (' ' + want + '\n') in symbols, want
    print('linked')


def main():
    check = sys.argv[1:] == ['--check']
    with tempfile.TemporaryDirectory() as name:
        tmp = Path(name)
        shutil.copytree(BASE, tmp / 'stage')
        for p in (HERE / 'overlay').rglob('*'):
            if p.is_file():
                d = tmp / 'stage' / p.relative_to(HERE / 'overlay')
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, d)
        (tmp / 'stage/tools/nf/cell').mkdir(parents=True, exist_ok=True)
        for name in DRIVER:
            shutil.copyfile(HERE / name, tmp / 'stage/tools/nf/cell' / name)
        # (the MHI core beside the board file, as the tested builds had it for #include)
        for p in (tmp / 'stage/wifi/lx/src').glob('mhi_host.*'):
            shutil.copyfile(p, tmp / 'stage/tools/nf/modem' / p.name)
        shutil.copyfile(VFS, tmp / 'vfs_attrlist.o')
        # (the radio's driver, built again against the headers this kernel's hk_qmi.o is built with: the shared one in xnu-deps is of 11-06, and
        # sys/hk_qmi.h's struct qmi_txn and qmi_handle grew on 11-09: ath11k's first QMI call then wrote over its own lists. HEAD's wifi/lx, tools/mk-ath11k.sh)
        shutil.copyfile(ATH, tmp / 'ath11k-cell.o')
        shutil.copyfile(__file__, tmp / 'link.py')
        w = run('docker', 'exec', CONTAINER, 'mktemp', '-d', '/tmp/cell-XXXXXXXX', capture_output=True).stdout.strip()
        assert w.startswith('/tmp/cell-'), w
        try:
            run('docker', 'cp', str(tmp) + '/.', CONTAINER + ':' + w)
            log = KERNEL / 'out' / ('cell-check.log' if check else 'cell-build.log')
            with log.open('w') as f:
                r = subprocess.run(['docker', 'exec', '-e', 'THEOS=/build/local-theos', '-e', 'W=' + w, CONTAINER, 'bash', '-ec',
                                    'source /build/xnu-env.sh; python3 "$W/link.py" inner' + (' --check' if check else '')],
                                   stdout=f, stderr=subprocess.STDOUT, text=True)
            if r.returncode != 0:
                print('FAILED: see', log)
                sys.exit(1)
            if check:
                print('the new units compile')
                return
            run('docker', 'cp', CONTAINER + ':' + w + '/candidate.mach', str(tmp / 'candidate.mach'))
            k = sha(tmp / 'candidate.mach')
            dest = KERNEL / 'out/kernels' / ('cell-' + k + '.mach')
            if not dest.exists():
                shutil.copyfile(tmp / 'candidate.mach', dest)
                dest.chmod(0o444)
            (KERNEL / 'out/cell-kernel.txt').write_text(str(dest) + '\n')
            print('kernel', k)
        finally:
            run('docker', 'exec', CONTAINER, 'python3', '-c', 'import shutil,sys;shutil.rmtree(sys.argv[1])', w)


if __name__ == '__main__':
    if sys.argv[1:2] == ['inner']:
        inner(Path(os.environ['W']), sys.argv[2:] == ['--check'])
    else:
        main()
