from pathlib import Path
import tempfile
import unittest

import gnss_settings_ticket as ticket


class AppDirectory(unittest.TestCase):
    def test_keeps_stock_controls_separate_from_diagnostic(self):
        base = dict(clean_return=True, teardown=True, pins_down=True, bundle_matches=True, bundle_visible=True,
                    original_directory_preserved=True, original_names_visible=True, stock_controls_verified=True,
                    settings_bundle_on_disk=True, settings_proxy_registered=True, signed_app_registered=True)
        self.assertTrue(ticket.complete(base))
        for key in ('original_directory_preserved','original_names_visible','stock_controls_verified',
                    'settings_proxy_registered','signed_app_registered'):
            with self.subTest(key=key):
                self.assertFalse(ticket.complete(dict(base, **{key:False})))

    def test_late_old_kernel_cannot_supply_stock_app_evidence(self):
        marker='== KERNEL LOG (the kernel is still running) ==\n'
        old=('app-view: stock_before=173 stock_after=173 settings_same=1 owned_bundle=1\n'
             'stock-apps: enum uid=501 bulk_base=141 bulk_staged=32 bulk_dup=0 bulk_errno=0 '
             'plain_base=141 plain_staged=32 plain_dup=0 plain_errno=0 end\n'
             'stock-apps: result uid=501 controls=7 total=7 pass=1 end\n')
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'fixture';p.write_text(marker+old+marker+'stock-apps: result uid=501 controls=0 total=7 pass=0 end\n')
            f=ticket.stock_evidence([p])
            self.assertFalse(f['original_directory_preserved'])
            self.assertFalse(f['original_names_visible'])
            self.assertFalse(f['stock_controls_verified'])

    def test_init_only_mounts_bundle(self):
        p=Path(__file__).resolve().parents[3]/'userland/src/init.c'
        init=p.read_text()
        self.assertNotIn('mount("bindfs", "/flash/Applications",',init)
        self.assertIn('bind_dir(in_image("/Applications/GNSSMonitor.app"), target)',init)


if __name__=='__main__':unittest.main()
