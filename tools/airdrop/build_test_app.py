"""Build a standalone iOS share-sheet source with only an ordinary app identifier."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import tarfile
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
K = HERE.parents[1]
OUT = K/'out/airdrop-ui/test-app'
BID = 'com.hackios.airdrop-test'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        stage=Path(tmp)
        app=stage/'Payload/AirDropTest.app'
        app.mkdir(parents=True)
        exe=app/'AirDropTest'
        recipe=('source /build/xnu-env.sh\n'
            'xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk '
            '-fobjc-arc -fblocks -Wall -Wextra -Werror -O2 -framework UIKit -framework Foundation -framework CoreGraphics '
            'tools/airdrop/TestApp.m -o "$1"')
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build',
                        'bash','-ec',recipe,'build',str(exe.relative_to(K))],check=True)
        info=dict(CFBundleDevelopmentRegion='en',CFBundleDisplayName='AirDrop Test',CFBundleName='AirDrop Test',
                  CFBundleExecutable=exe.name,CFBundleIdentifier=BID,CFBundlePackageType='APPL',
                  CFBundleVersion='1',CFBundleShortVersionString='1.0',CFBundleInfoDictionaryVersion='6.0',
                  CFBundleSupportedPlatforms=['iPhoneOS'],MinimumOSVersion='16.5',UIDeviceFamily=[1],
                  LSRequiresIPhoneOS=True,UILaunchScreen={})
        (app/'Info.plist').write_bytes(plistlib.dumps(info))
        (app/'PkgInfo').write_bytes(b'APPL????')
        exe.chmod(0o755)
        entitlements=stage/'ents'/(BID+'.plist')
        entitlements.parent.mkdir()
        entitlements.write_bytes(plistlib.dumps({'application-identifier':BID}))
        subprocess.run(['codesign','--force','--sign','-','--identifier',BID,'--entitlements',str(entitlements),
                        '--generate-entitlement-der','--timestamp=none',str(app)],check=True)
        subprocess.run(['codesign','--verify','--strict','--deep',str(app)],check=True)
        actual=subprocess.check_output(['codesign','-d','--entitlements',':-',str(app)],stderr=subprocess.DEVNULL)
        assert plistlib.loads(actual)==plistlib.loads(entitlements.read_bytes())
        detail=subprocess.check_output(['codesign','-dvvv',str(app)],stderr=subprocess.STDOUT,text=True)
        cdhash=re.search(r'^CDHash=([0-9a-f]+)$',detail,re.M)[1]
        target=OUT/cdhash
        target.mkdir(exist_ok=True)
        ipa=target/'AirDropTest.ipa'
        with zipfile.ZipFile(ipa,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(app.rglob('*')):
                if path.is_file(): archive.write(path,str(path.relative_to(stage)))
        root_tar=target/'AirDropTest.tar'
        def normalized(member):
            member.uid=member.gid=0
            member.uname=member.gname=''
            member.mtime=0
            member.mode=0o755 if member.isdir() or member.name.endswith('/AirDropTest') else 0o644
            return member
        with tarfile.open(root_tar,'w',format=tarfile.USTAR_FORMAT) as tar:
            for name in ('Payload','ents'): tar.add(stage/name,arcname=name,filter=normalized)
        with tarfile.open(root_tar) as tar:
            assert tar.extractfile('Payload/AirDropTest.app/AirDropTest').read()==exe.read_bytes()
            assert plistlib.loads(tar.extractfile('ents/'+BID+'.plist').read())==plistlib.loads(actual)
        receipt=dict(bundle_id=BID,cdhash=cdhash,signer='ad-hoc',entitlement_keys=list(plistlib.loads(actual)),
                     executable_sha256=sha(exe),executable_bytes=exe.stat().st_size,ipa=str(ipa),
                     ipa_sha256=sha(ipa),ipa_bytes=ipa.stat().st_size,tar=str(root_tar),
                     tar_sha256=sha(root_tar),tar_bytes=root_tar.stat().st_size,
                     payload_name='AirDrop-test.txt',payload_bytes=len(b'HackiOS AirDrop test\n')+36+1,
                     runtime_digest='SHA-256 displayed and logged for each generated file')
        (OUT/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps(receipt,indent=2))

if __name__=='__main__':
    main()
