import tempfile
from pathlib import Path
import unittest
import gnss_settings_gate as gate
m=dict(executable_sha256='a'*64,plist_sha256='b'*64)
own='gnss-ls seq=0 uid=501 settings=1 id=1 installed=1 byurl=1 path=/Applications/GNSSMonitor.app exe='+'a'*64+' plist='+'b'*64+' end'
stock='gnss-settings-ls seq=0 uid=501 stock_plist=1 stock_id=1 stock_exec=1 proxy_id=1 proxy_installed=1 proxy_path=stock by_url=1 end'
event='gnss-settings-action seq=0 uid=501 pid=17 attempt=1 tap_ms=60000 end_ms=60100 accepted=1 end'
active='new scene host settings: contextID:17 <sceneID:com.hackios.gnss-monitor-scene> unspecified -> foreground'
prefs='new scene host settings: contextID:18 <sceneID:com.apple.Preferences-scene> unspecified -> foreground'
class Settings(unittest.TestCase):
    def report(self,*lines):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'capture';p.write_text('\n'.join(lines))
            return gate.collect([p,p],m)
    def test_normal_public_handoff(self):
        r=self.report(own,stock,active,event,prefs)
        self.assertTrue(r['settings_bundle_on_disk'] and r['settings_proxy_registered'] and r['settings_user_action'])
        self.assertEqual(r['settings_public_open'],'accepted');self.assertTrue(r['settings_public_handoff_observed'])
    def test_independent_registration(self):
        r=self.report(own.replace('settings=1','settings=0'),stock.replace('proxy_installed=1','proxy_installed=0'),active)
        self.assertTrue(r['signed_app_registered'] and r['signed_app_foreground'] and r['settings_bundle_on_disk'])
        self.assertFalse(r['settings_proxy_registered']);self.assertEqual(r['settings_public_open'],'untested')
    def test_no_click_or_refused(self):
        self.assertFalse(self.report(own,stock,active,prefs)['settings_public_handoff_observed'])
        self.assertFalse(self.report(own,stock,active,event)['settings_public_handoff_observed'])
        self.assertFalse(self.report(own,stock,prefs,active,event)['settings_public_handoff_observed'])
        self.assertEqual(self.report(own,stock,active,event.replace('accepted=1','accepted=0'))['settings_public_open'],'refused')
    def test_conflict_or_bad_source(self):
        with self.assertRaises(ValueError):self.report(own,stock,event,event.replace('accepted=1','accepted=0'))
        with self.assertRaises(ValueError):self.report(own.replace(' id=1',' id=0'),stock,event)
    def test_old_237_is_no_stock_or_settings_action_proof(self):
        r=self.report(own.replace('settings=1','settings=0'),active)
        self.assertTrue(r['signed_app_registered']);self.assertFalse(r['settings_bundle_on_disk'] or r['settings_proxy_registered'])
        self.assertEqual(r['settings_public_open'],'untested')
if __name__=='__main__':unittest.main()
