"""Offline state-machine checks, including a late decision and modal refusal."""
import unittest

from watch import Mark, Route, classify, swipe, tap, wireless_bounds, completed_since

def marks(*values):
    return [Mark(k,x,y,label) for k,x,y,label in values]

HOME = marks(('facetime',.15,.39,'FaceTime'),('calendar',.39,.39,'Calendar'),('photos',.62,.39,'Photos'),
             ('clock',.62,.52,'Clock'),('settings',.63,.75,'Settings'))
ICON = marks(('weather',.15,.22,'Weather'),('stocks',.39,.22,'Stocks'),('find_my',.63,.22,'Find My'),
             ('home_app',.85,.22,'Home'),('monitor',.85,.55,'Cellular Monitor'))
APP = marks(('monitor',.25,.10,'Cellular Monitor'),('live',.21,.17,'Live modem'),('sample',.27,.19,'Sample 11'),
            ('sim_section',.20,.22,'SIM and USIM'),('gw_section',.19,.36,'Primary GW'),
            ('activation_section',.19,.46,'Activation'),('radio_section',.20,.62,'Radio and data'),
            ('open_settings',.50,.86,'Open Settings'))
SETTINGS = marks(('settings',.25,.14,'Settings'),('airplane',.34,.33,'Airplane Mode'),('wifi',.20,.40,'Wi-Fi'),
                 ('bt',.22,.47,'Bluetooth'),('notifications',.27,.59,'Notifications'))
def decide(route,t,rows):
    return route.decide(t,rows,'frame'+str(t),'image_hash')

class RouteTests(unittest.TestCase):
    def test_iphont_gesture_durations_are_seconds(self):
        for cmd in (swipe(.85,.53,.15,.53),tap(.80,.48)):
            duration=float(cmd[cmd.index('--duration')+1])
            self.assertGreater(duration,0)
            self.assertLess(duration,1.0)

    def test_normal_route(self):
        r=Route(277)
        self.assertIsNone(decide(r,1,HOME))
        self.assertEqual(decide(r,2,HOME)[0][0],'swipe')
        self.assertIsNone(decide(r,3,ICON))
        self.assertEqual(decide(r,4,ICON)[0][0],'tap')
        self.assertIsNone(decide(r,5,APP))
        accepted=APP+marks(('accepted',.25,.51,'Modem accepted'))
        self.assertIsNone(decide(r,7,accepted))  # ACK alone is never verified.
        terminal=APP+marks(('verified',.25,.51,'Verified by readback'))
        self.assertIsNone(decide(r,10,terminal))
        self.assertIsNone(decide(r,13,terminal))  # Capture subsequent samples/visible fields.
        self.assertEqual(decide(r,23,terminal)[1],'project App Open Settings')
        self.assertIsNone(decide(r,25,SETTINGS))
        self.assertIsNone(decide(r,27,SETTINGS))
        self.assertTrue(r.finished)
        self.assertTrue(r.info['project_app_visible'] and r.info['live_sample_observed'])
        self.assertTrue(r.info['wireless_group_bracketed'])
        self.assertIsNone(r.info['native_cellular_row_visible'])  # Human confirmation still required.
    def test_genuine_row_and_detail(self):
        r=Route(277);r.settings_at=1
        cell=SETTINGS+marks(('cellular',.27,.52,'Cellular'))
        decide(r,10,cell)
        self.assertEqual(decide(r,11,cell)[1],'visible stock Cellular navigation label')
        unrelated=marks(('cellular',.50,.09,'Cellular'),('settings',.09,.09,'Settings'))
        self.assertEqual(classify(unrelated),'cellular_pane')
        decide(r,14,unrelated);decide(r,16,unrelated)
        self.assertTrue(r.info['native_cellular_pane_visible'])
    def test_dialog_never_tapped(self):
        r=Route(277)
        self.assertIsNone(decide(r,1,APP+marks(('modal',.50,.60,'Modal'))))
        self.assertTrue(r.finished)
    def test_incomplete_wireless_group_not_negative(self):
        self.assertIsNone(wireless_bounds([m for m in SETTINGS if m.kind!='airplane']))
    def test_unbracketed_cellular_label_not_tapped(self):
        r=Route(277);r.settings_at=1
        bottom=SETTINGS+marks(('cellular',.25,.80,'Cellular'))
        decide(r,1,bottom);action=decide(r,2,bottom)
        self.assertNotEqual(action[0][0] if action else '', 'tap')
    def test_35_second_hold_without_terminal(self):
        r=Route(277);decide(r,5,APP)
        self.assertIsNone(decide(r,39,APP))
        self.assertEqual(decide(r,40,APP)[1],'project App Open Settings')
    def test_online_mode_keeps_online_frame_and_ignores_completed_uim_alone(self):
        r=Route(278,observe_online=True)
        offline=APP+marks(('already_bound',.3,.55,'Already bound'),('dms_data',.3,.64,'dms offline'),
            ('nas_data',.3,.68,'nas unregistered'),('wds_data',.3,.74,'wds disconnected'))
        self.assertIsNone(decide(r,5,offline))
        self.assertIsNone(decide(r,20,offline))
        online=APP+marks(('dms_data',.3,.64,'dms online'),('nas_data',.3,.68,'nas registered'),
            ('cs_attach',.3,.68,'cs attached'),('ps_attach',.4,.68,'ps attached'),('wds_data',.3,.74,'wds disconnected'))
        self.assertIsNone(decide(r,21,online))
        self.assertEqual(decide(r,27,online)[1],'project App Open Settings')
        self.assertIn('online_nas_wds_frame',r.info)
        self.assertNotIn('roaming',str(r.info['captured_app_frames'][-1]))
    def test_online_mode_bounds_offline_without_marking_online(self):
        r=Route(278,observe_online=True)
        offline=APP+marks(('dms_data',.3,.65,'dms offline'))
        self.assertIsNone(decide(r,10,offline))
        self.assertIsNone(decide(r,50,offline))
        r.online_fallback_not_before=195
        self.assertIsNone(decide(r,75,offline))
        self.assertEqual(decide(r,195,offline)[1],'project App Open Settings')
        self.assertNotIn('online_frame',r.info)
    def test_earlier_day_same_run_name_does_not_end_watcher(self):
        import os,tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)/'183555-run.log';p.touch()
            os.utime(p,(10,10));self.assertFalse(completed_since(p,100000))
            os.utime(p,(100001,100001));self.assertTrue(completed_since(p,100000))
    def test_missing_sysctl_never_becomes_live(self):
        unavailable=[x for x in APP if x.kind not in ('live','sample')]+marks(('waiting',.2,.17,'Waiting for modem'))
        r=Route(277);decide(r,5,unavailable);a=decide(r,51,unavailable)
        self.assertFalse(r.info['live_sample_observed'])
        self.assertEqual(a[1],'project App Open Settings')

if __name__=='__main__':unittest.main()
