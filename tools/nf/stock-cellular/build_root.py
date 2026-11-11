"""Host-only builder for the 20F66 stock cellular UI observation RAM add-on."""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
K=HERE.parents[2]
AUDIT=K.parent/'oneplus8-audit'
OUT=AUDIT/'stock-cellular-ui'
HOME=AUDIT/'gnss-settings-next/bulk-union/home-view'
sys.path.insert(0,str(AUDIT/'metal-bringup-next'))
from prepare_ab import sha, verify

ARGS=('fb_panel=amb655uv01 fb_hz=90 cpus=8 bootcpu=7 gpu=1 board_gpu_zap=0x8e21a000 '
      'wifi=0 adsp=0 bt=0 sound=0 sound_amps=0 haptic=0 cam=0 torch=0 syssleep=0 '
      'ufs=2 ufs_write=0 root_apfs=ufs_darwin hk_data=2 hk_data_mb=1024 var_store=0 '
      'var_data=hfs var_data_format=1 modem=3 modem_loc=0 modem_gnss=0 modem_radio=6 modem_mission=1 iphont=1 iphont_control=1 battery_trace=1')
REMOVED=('LaunchDaemons/com.hackios.demo.plist','LaunchDaemons/com.hackios.wifi.plist',
         'LaunchDaemons/com.iphont.agent.plist','LaunchDaemons/com.apple.wifid.plist',
         'LaunchDaemons/com.hackios.prefs.plist','LaunchDaemons/com.apple.locationd.plist',
         'LaunchDaemons/com.apple.locationpushd.plist','fw/network','fw/mac','fw/amss.bin',
         'fw/m3.bin','fw/qca6390-bdwlan.elf','etc/rc.jobs','bin/iphontd','bin/hkhttpd','bin/gnss_monitor','bin/radio_observe')

def build(frontend=False,activation=False,history=False,selection=False):
    os.umask(0o077)
    OUT.mkdir(mode=0o700,exist_ok=True)
    binary=OUT/'stock_cellular_ui'
    tmp=subprocess.check_output(['docker','exec','oneplus8-build','mktemp','-d','/tmp/stock-cellular-ui.XXXXXXXX'],text=True).strip()
    assert tmp.startswith('/tmp/stock-cellular-ui.')
    script='. /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation -framework CoreFoundation -framework IOKit -o "$1/probe" tools/nf/stock-cellular/observe.m'
    subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build',
                    'bash','-ec',script,'bash',tmp],check=True)
    subprocess.run(['docker','cp','oneplus8-build:'+tmp+'/probe',str(binary)],check=True)
    folder=OUT/'selection-ui' if selection else OUT/'history-ui' if history else OUT/'activation-ui' if activation else OUT/'qmux-frontend' if frontend else OUT
    folder.mkdir(mode=0o700,exist_ok=True)
    if frontend:
        command='. /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -Wall -Wextra -Werror -O2 -framework CoreFoundation -framework IOKit -o "$1/registry" tools/nf/stock-cellular/frontend_registry.c'
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build','bash','-ec',command,'bash',tmp],check=True)
        subprocess.run(['docker','cp','oneplus8-build:'+tmp+'/registry',str(folder/'qmux_registry')],check=True)
    signed=json.loads((HOME/'direct/profile.json').read_text());init=HOME/'init' 
    assert sha(init)==signed['init_sha256']
    radio=json.loads((K/'out/eye/uim-endpoints-ticket-267/result.json').read_text())
    assert radio['ticket']==267 and radio['kernel']=='4764013dd575065f92541439739bffa2a3036452d94f59ca387e073bc27025dc'
    kernel=K/'out/kernels'/f'nf-uim-endpoints-{radio["kernel"]}.mach'
    assert sha(kernel)==radio['kernel']
    abp=OUT/'abp_match'
    subprocess.run([str(HERE/'build_abp_match.sh'),str(abp)],check=True)
    assert sha(abp)=='3d41216e223c7873d7999bc979cfb88ff9277b1cd888a0f3a1eea55c03d99ffc'
    relative_job='LaunchDaemons/com.hackios.stock-cellular-ui.plist'
    with tempfile.TemporaryDirectory(prefix='cell-desktop-',dir=OUT) as temp:
        t=Path(temp);tree=t/'root';shutil.copytree(K/'out/radio-state-root',tree,symlinks=True);tree.chmod(0o755)
        for f in tree.rglob('*'):
            if f.is_symlink():os.chmod(f,0o777,follow_symlinks=False)
        for name in REMOVED:(tree/name).unlink(missing_ok=True)
        for src,rel,mode in ((init,'sbin/init',0o755),(binary,'bin/stock_cellular_ui',0o755),(abp,'bin/abp_match',0o755),(HERE/Path(relative_job).name,relative_job,0o644)):
            shutil.copyfile(src,tree/rel);(tree/rel).chmod(mode)
        if frontend:
            for src, rel, mode in ((folder/'qmux_registry','bin/qmux_registry',0o755),
                    (HERE/'com.hackios.qmux-registry.plist','LaunchDaemons/com.hackios.qmux-registry.plist',0o644)):
                shutil.copyfile(src,tree/rel)
                (tree/rel).chmod(mode)
        assert all(not (tree/path).exists() for path in REMOVED) and not (tree/'GNSSApplications').exists()
        jobs={f.name:plistlib.loads(f.read_bytes()) for f in (tree/'LaunchDaemons').glob('*.plist')}
        assert 'com.apple.SpringBoard.plist' in jobs and 'com.apple.backboardd.plist' in jobs
        assert not any('com.apple.commcenter.coretelephony.xpc' in j.get('MachServices',{}) for j in jobs.values())
        assert jobs[Path(relative_job).name]['UserName']=='mobile' and not jobs[Path(relative_job).name].get('KeepAlive')
        assert (tree/'etc/rc').read_text().strip().endswith('exec /sbin/launchd.ios')
        if activation:
            signed_app=json.loads((folder/'signed.json').read_text());app=Path(signed_app['bundle'])
            assert app.name=='GNSSMonitor.app' and signed_app['standard_development_signing'] and not signed_app['private_entitlements']
            assert sha(app/'GNSSMonitor')==signed_app['executable_sha256'] and sha(app/'Info.plist')==signed_app['plist_sha256']
            upper=tree/'GNSSApplications';upper.mkdir(mode=0o755);upper.chmod(0o755)
            shutil.copytree(app,upper/app.name)
            subprocess.run(['codesign','--verify','--deep','--strict',str(upper/app.name)],check=True)
            assert [p.name for p in upper.iterdir()]==[app.name] and not (tree/'Applications').exists()
        img=t/'root.apfs';subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(img),'hackios='+str(tree)],check=True,stdout=subprocess.DEVNULL)
        readback=verify(img,tree);digest=sha(img);target=K/'out'/f'stock-cellular-ui-{digest}.apfs'
        if not target.exists():shutil.copyfile(img,target);target.chmod(0o400)
        assert sha(target)==digest
        prefix=(AUDIT/'gpu-plan/observed-loader-prefix.txt').read_bytes()
        size=len(prefix)+len('loader_return_after=260 epoch=0000000000 sb_delay=8 '+ARGS)
        mac=K/'fw-wifi/mac'
        if mac.is_file():size+=len(' wifi_mac=')+len(b''.join(mac.read_bytes().split()))
        assert size<=607
        manifest=dict(root=str(target),root_sha256=digest,kernel=str(kernel),kernel_sha256=sha(kernel),
                      probe_sha256=sha(binary),init_sha256=sha(init),jobs=len(jobs),removed=list(REMOVED),
                      stock_system='read-only ufs_darwin via flashboot.apfs; original SpringBoard, Preferences and SettingsCellular',
                      resident_sha256=sha(K/'userland/flashboot.apfs'),cmdline=ARGS,cmdline_bytes=size,seconds=260,
                      check=readback,source_control_ticket=258,modem_control_ticket=267,abp_sha256=sha(abp))
        if frontend:
            manifest.update(registry_probe_sha256=sha(folder/'qmux_registry'),root_only=True)
            for key in ('kernel','kernel_sha256','cmdline','cmdline_bytes','seconds'):
                manifest.pop(key,None)
        if activation:
            manifest.update(app_cdhash=signed_app['cdhash'],app_executable_sha256=signed_app['executable_sha256'],
                            app_plist_sha256=signed_app['plist_sha256'],app_identifier=signed_app['bundle_id'],display_name=signed_app['display_name'],
                            snapshot_header_sha256=signed_app['header_sha256'],requires_home_apps=True,root_only=True)
            for key in ('kernel','kernel_sha256','cmdline','cmdline_bytes','seconds'):
                manifest.pop(key,None)
        if history:manifest.update(history_api=signed_app['history_api'],history_header_sha256=signed_app['history_header_sha256'])
        if selection:manifest.update(selection_api=signed_app['selection_api'],selection_header_sha256=signed_app['selection_header_sha256'])
        (folder/('root.json' if frontend or activation else 'profile.json')).write_text(json.dumps(manifest,indent=2)+'\n')
        print(digest,sha(binary),size,readback,flush=True)
if __name__=='__main__':
    args=sys.argv[1:]
    if args not in ([],['--frontend'],['--activation-ui'],['--activation-history'],['--selection']):raise SystemExit('unknown cellular UI root variant')
    selection=args==['--selection']
    history=selection or args==['--activation-history']
    build(frontend=args==['--frontend'],activation=history or args==['--activation-ui'],history=history,selection=selection)
