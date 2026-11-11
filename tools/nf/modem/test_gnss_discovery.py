import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest

import gnss_discovery as d

class Discovery(unittest.TestCase):
    def setUp(self):
        self.m,_,_,_=d.spec()
    def observe(self,lines):
        with tempfile.TemporaryDirectory() as tmp:
            f=Path(tmp)/'native.log';f.write_text('\n'.join(lines))
            return d.collect([f,f],self.m)
    def row(self,seq=0,path='/Applications/GNSSMonitor.app',settings=1,identifier=1,installed=1,url=1,matching=True):
        exe=self.m['executable_sha256'] if matching else 'missing'
        plist=self.m['plist_sha256'] if matching else 'missing'
        return f'gnss-ls seq={seq} uid=501 settings={settings} id={identifier} installed={installed} byurl={url} path={path} exe={exe} plist={plist} end'
    def test_registration_vs_visible_path(self):
        r=self.observe([self.row(),self.row(1)]);self.assertTrue(r['normal_registered']);self.assertFalse(r['normal_native_foreground'])
        r=self.observe([self.row(path='none',identifier=0,installed=-1,url=1,matching=False)])
        self.assertEqual(r['discovery_classification'],'no-registered-identifier')
        r=self.observe([self.row(matching=False)]);self.assertFalse(r['normal_registered'])
    def test_user_launch_requires_actual_scene(self):
        r=self.observe([self.row(),self.row(1),'[TouchEvents] new scene host settings: contextID:11 <sceneID:com.hackios.gnss-monitor-test> unspecified -> foreground'])
        self.assertTrue(r['normal_native_foreground']);self.assertFalse(r['core_request'])
    def test_broken_control_unknown_and_bad_paths(self):
        r=self.observe([self.row(settings=-1)])
        self.assertEqual(r['discovery_classification'],'lookup-inconclusive')
        with self.assertRaises(ValueError):self.observe([self.row(),self.row(path='none',matching=False)])
        with self.assertRaises(ValueError):self.observe([self.row(path='/private/var/mobile/Documents/file')])
    def test_registered_user_container(self):
        p='/private/var/containers/Bundle/Application/11111111-2222-3333-4444-555555555555/GNSSMonitor.app'
        r=self.observe([self.row(path=p)]);self.assertEqual(r['registered_path'],p)
    def test_baseline_is_not_a_repaired_run(self):
        p=d.ROOT/'logs/052622-run.log.live'
        if not p.is_file():self.skipTest('local prior capture missing')
        r=d.collect([p],self.m);self.assertTrue(r['legacy_launch_requested'])
        self.assertFalse(r['normal_native_foreground']);self.assertFalse(r['bundle_matches']);self.assertFalse(r['normal_registered'])
    def test_pinned_apple_development_app(self):
        audit=d.ROOT.parent/'oneplus8-audit/gnss-launch-next'
        signing=json.loads((audit/'signed.json').read_text());app=Path(signing['bundle'])
        self.assertEqual(signing['cdhash'],self.m['bundle_cdhash']);self.assertEqual(signing['profile_sha256'],self.m['issuer_profile_sha256'])
        subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
        ent=plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(app)],stderr=subprocess.DEVNULL))
        self.assertEqual(set(ent),{'application-identifier','com.apple.developer.team-identifier'})
        info=plistlib.loads((app/'Info.plist').read_bytes());self.assertIn('NSLocationWhenInUseUsageDescription',info)
        self.assertEqual(info['CFBundleURLTypes'][0]['CFBundleURLSchemes'],['hackios-gnss-monitor'])
        self.assertFalse(signing['paired_device_eligibility_verified'])
if __name__=='__main__':unittest.main()
