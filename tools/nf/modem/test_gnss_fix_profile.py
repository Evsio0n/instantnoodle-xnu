import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import gnss_fix_ticket as profile

class GnssProfile(unittest.TestCase):
    def test_artifacts_and_current_board_guard(self):
        m,k,_,n=profile.spec()
        self.assertLessEqual(n,607)
        self.assertIn(b'WDA format',k.read_bytes())
        for args in ('modem_gnss=2 wifi=1','modem_gnss=2','wifi=1 modem_gnss=2 wifi=0'):
            result=subprocess.run(['bash',str(profile.ROOT/'tools/nf/boot.sh'),'guard',args,'unused','125'],
                        cwd=profile.ROOT,env=dict(os.environ,NF_KERNEL=str(k)),capture_output=True,text=True,timeout=8)
            self.assertNotEqual(result.returncode,0);self.assertIn('require',result.stdout)
        with patch.object(profile,'ARGS',profile.ARGS+' wifi=1'),self.assertRaises(ValueError):
            profile.spec()
    def test_public_client(self):
        code=(profile.ROOT/'userland/src/gnss_monitor.m').read_text()
        p=plistlib.loads((profile.ROOT/'userland/src/gnss_monitor.plist').read_bytes())
        self.assertIn('NSLocationWhenInUseUsageDescription',p)
        self.assertNotIn('NSLocationAlwaysAndWhenInUseUsageDescription',p)
        self.assertNotIn('UIBackgroundModes',p)
        for private in ('initWithEffectiveBundleIdentifier','setAuthorizationStatusByType','setLocationServicesEnabled','startUpdatingLocation'):
            self.assertNotIn(private,code)
        worker=code.split('- (void)permission:(NSDictionary *)action {',1)[1].split('- (void)position:',1)[0]
        button=code.split('- (void)requestPermission {',1)[1].split('- (void)locationBecameActive:',1)[0]
        self.assertIn('UIApplicationStateActive',button)
        self.assertIn('onThread:self.locationThread',button)
        self.assertNotIn('self.manager.authorizationStatus',worker)
        self.assertNotIn('locationServicesEnabled',worker)
        self.assertIn('valid:action generation:self.consentGeneration',worker)
        self.assertIn('requestWhenInUseAuthorization',worker)
    def test_permission_is_independent_and_retryable(self):
        code=(profile.ROOT/'userland/src/gnss_monitor.m').read_text()
        section=code.split('- (void)requestPermission {',1)[1].split('- (void)locationBecameActive:',1)[0]
        self.assertIn('@selector(permission:)',section)
        startup=code.split('- (void)viewWillAppear:(BOOL)animated',1)[1].split('- (void)viewDidDisappear:',1)[0]
        self.assertFalse(any(api in startup for api in ('authorizationStatus','CLLocationManager','NSUserDefaults','radioStatus()','fixQuality()','gnssStatus()')))
        self.assertIn('permissionInFlight',section)
        self.assertNotIn('requestLocation]',section)
        self.assertNotIn('wantsLocation = YES',section)
        self.assertIn('afterDelay:15',section)
        resources=profile.ROOT/'userland/src/gnss_monitor_resources'
        settings=plistlib.loads((resources/'Settings.bundle/Root.plist').read_bytes())['PreferenceSpecifiers']
        switch=next(item for item in settings if item['Type']=='PSToggleSwitchSpecifier')
        self.assertEqual(switch['Key'],'ShowRadioSummary')
        self.assertNotIn('location',switch['Key'].lower())
        self.assertIn('[NSUserDefaults.standardUserDefaults objectForKey:@"ShowRadioSummary"]',code)
        for lang in ('en','zh-Hans','zh-Hant'):
            self.assertIn('NSLocationWhenInUseUsageDescription',(resources/(lang+'.lproj')/'InfoPlist.strings').read_text())
    def test_actual_bundle_and_consent_evidence(self):
        m,_,_,_=profile.spec()
        text=('''[ ok ] GNSSMonitor bundle visible under /Applications on the RAM profile
battery_test: SpringBoard was asked to launch com.hackios.gnss-monitor: it does (0)
backboardd[17] new scene host settings: contextID:A <sceneID:com.hackios.gnss-monitor-record> unspecified -> foreground
GNSSMonitor: private-fix profile active; system request requires a foreground tap
gnss-fix-quality seq=1 state=1 valid_count=0 age_ms=0 end
''' +f'gnss-bundle-sha executable={m["executable_sha256"]} plist={m["plist_sha256"]} end\n')
        with tempfile.TemporaryDirectory() as w:
            f=Path(w)/'log';f.write_text(text);r=profile.evidence([f,f],m)
            self.assertTrue(r['scene_foreground'] and r['bundle_visible'] and r['bundle_matches'] and r['launch_accepted'])
            self.assertFalse(r['owner_tapped'] or r['core_request'] or r['core_reply'] or r['satellite_fix_seen'])
            f.write_text(text+'gnss-fix-quality seq=2 state=2 valid_count=1 age_ms=400 end\n')
            self.assertTrue(profile.evidence([f],m)['satellite_fix_seen'])
            f.write_text(text.replace('[ ok ]','[FAIL]').replace(m['executable_sha256'],'bad'))
            r=profile.evidence([f],m);self.assertFalse(r['bundle_visible'] or r['bundle_matches'])
    def test_driver(self):
        with tempfile.TemporaryDirectory() as w:
            exe=Path(w)/'fixture'
            subprocess.run(['clang','-std=c11','-Werror','-Wall','-Wextra','-fsanitize=address,undefined',str(Path(__file__).with_name('test_gnss_fix.c')),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
