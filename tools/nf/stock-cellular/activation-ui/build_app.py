"""Build the offline cellular-only owner snapshot viewer and sign it with the existing Apple-issued profile."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
K=HERE.parents[3]
AUDIT=K.parent/'oneplus8-audit'
OUT=AUDIT/'stock-cellular-ui/activation-ui'
HEADER=AUDIT/'cellular-data-next/uim-activate-once/hk_cellular_observation.h'
PREVIOUS=AUDIT/'gnss-settings-next/sign_owned.py'
BID='com.hackios.gnss-monitor'

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run(cmd,**opts):
    return subprocess.run(cmd,check=True,**opts)

def main(history=False,selection=False):
    out=OUT.parent/'selection-ui' if selection else (OUT.parent/'history-ui') if history else OUT
    os.umask(0o077);out.mkdir(parents=True,mode=0o700,exist_ok=True)
    spec=importlib.util.spec_from_file_location('existing_gnss_signing',PREVIOUS)
    signing=importlib.util.module_from_spec(spec);spec.loader.exec_module(signing)
    exact,expiry,profile,signer,prefix,team=signing.profiles()
    assert signing.BID==BID
    remote=subprocess.check_output(['docker','exec','oneplus8-build','mktemp','-d','/tmp/cellular-status-ui.XXXXXXXX'],text=True).strip()
    assert remote.startswith('/tmp/cellular-status-ui.')
    try:
        run(['docker','cp',str(HEADER),'oneplus8-build:'+remote+'/hk_cellular_observation.h'])
        command='. /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -fobjc-arc -fblocks -O2 -Wall -Wextra -Werror -I"$1" -framework Foundation -framework UIKit -o "$1/GNSSMonitor" tools/nf/stock-cellular/activation-ui/CellularMonitor.m'
        if history:command=command.replace('-fobjc-arc','-DHK_SHOW_CELLULAR_HISTORY=1 -fobjc-arc')
        if selection:command=command.replace('-fobjc-arc','-DHK_SHOW_CELLULAR_SELECTION=1 -fobjc-arc')
        run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build','bash','-ec',command,'bash',remote])
        with tempfile.TemporaryDirectory(dir=out) as tmp:
            stage=Path(tmp);app=stage/'GNSSMonitor.app';app.mkdir()
            run(['docker','cp','oneplus8-build:'+remote+'/GNSSMonitor',str(app/'GNSSMonitor')])
            (app/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleDevelopmentRegion='en',CFBundleDisplayName='Cellular Monitor',
                CFBundleExecutable='GNSSMonitor',CFBundleIdentifier=BID,CFBundleInfoDictionaryVersion='6.0',CFBundleName='Cellular Monitor',
                CFBundlePackageType='APPL',CFBundleShortVersionString='0.9' if selection else '0.8' if history else '0.7',CFBundleVersion='9' if selection else '8' if history else '7',UIDeviceFamily=[1],LSRequiresIPhoneOS=True,
                MinimumOSVersion='16.5',CFBundleSupportedPlatforms=['iPhoneOS'],UILaunchScreen={},UIRequiresFullScreen=True,
                UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait'])))
            (app/'PkgInfo').write_bytes(b'APPL????');shutil.copyfile(profile,app/'embedded.mobileprovision')
            allowed={'application-identifier':prefix+'.'+BID,'com.apple.developer.team-identifier':team}
            (stage/'entitlements.plist').write_bytes(plistlib.dumps(allowed))
            for f in app.rglob('*'):f.chmod(0o755 if f.is_dir() or f.name=='GNSSMonitor' else 0o644)
            app.chmod(0o755)
            run(['codesign','--force','--sign',signer,'--identifier',BID,'--entitlements',str(stage/'entitlements.plist'),'--generate-entitlement-der','--timestamp=none',str(app)],timeout=70)
            run(['codesign','--verify','--strict','--deep',str(app)])
            actual=plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(app)],stderr=subprocess.DEVNULL))
            assert actual==allowed
            run(['codesign','-d','--extract-certificates='+str(stage/'leaf'),str(app)],stderr=subprocess.DEVNULL)
            assert hashlib.sha1((stage/'leaf0').read_bytes()).hexdigest()==signer
            detail=subprocess.check_output(['codesign','-dvvv',str(app)],stderr=subprocess.STDOUT,text=True)
            cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',detail,re.M)[1]
            dest=out/'signed'/cdhash/'GNSSMonitor.app';dest.parent.mkdir(parents=True,exist_ok=True)
            if not dest.exists():shutil.copytree(app,dest)
            run(['codesign','--verify','--strict','--deep',str(dest)])
            binary=dest/'GNSSMonitor'
            assert sha(binary)==sha(app/'GNSSMonitor')
            deps=subprocess.check_output(['otool','-L',str(binary)],text=True)
            assert 'CoreLocation' not in deps and 'CoreTelephony' not in deps
            receipt=dict(bundle=str(dest),bundle_id=BID,display_name='Cellular Monitor',cdhash=cdhash,
                executable_sha256=sha(binary),plist_sha256=sha(dest/'Info.plist'),issuer_profile_sha256=sha(dest/'embedded.mobileprovision'),
                header_sha256=sha(HEADER),app_source_sha256=sha(HERE/'CellularMonitor.m'),standard_development_signing=True,
                private_entitlements=False,snapshot_name='kern.hackios.cellular_observation',ui_can_activate=False)
            if history:receipt.update(history_api='kern.hackios.cellular_history',history_header_sha256=sha(HERE/'history/hk_cellular_history.h'))
            if selection:receipt.update(selection_api='kern.hackios.cellular_selection',selection_header_sha256=sha(HERE/'selection/hk_cellular_selection.h'))
            (out/'signed.json').write_text(json.dumps(receipt,indent=2)+'\n')
            print(cdhash,sha(binary),sha(HEADER))
    finally:run(['docker','exec','oneplus8-build','rm','-rf','--',remote])
if __name__=='__main__':
    assert sys.argv[1:] in ([],['--history'],['--selection'])
    selection=sys.argv[1:]==['--selection']
    history=selection or sys.argv[1:]==['--history']
    # The retained profile lookup module also inspects argv when imported.
    del sys.argv[1:]
    main(history,selection)
