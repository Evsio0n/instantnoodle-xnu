import subprocess
import unittest
from unittest.mock import patch
import cold_recover as c


class StockRecovery(unittest.TestCase):
    def test_delayed_disconnect_and_stock_product_verified(self):
        states = [{c.LOADER},{c.LOADER},set(),{c.STOCK},{c.STOCK},set(),{c.LOADER}]
        with patch.object(c,"devices",side_effect=states),patch.object(c.time,"sleep"),patch.object(c,"variable",side_effect=["","kona","idle"]) as var,patch.object(c,"fastboot",return_value="OKAY") as fb:
            c.recover(True,False,30)
            self.assertEqual(fb.call_args_list[0].args,("-s",c.LOADER,"reboot","bootloader"))
            self.assertEqual(fb.call_args_list[1].args[:3],("-s",c.STOCK,"boot"))
            self.assertEqual(len(fb.call_args_list),2)
            self.assertEqual(var.call_args_list[-1].args,(c.LOADER,"result"))

    def test_stock_only_never_uploads(self):
        with patch.object(c,"devices",return_value={c.STOCK}),patch.object(c,"variable",return_value="kona"),patch.object(c,"fastboot") as fb:
            c.recover(False,True,3)
            fb.assert_not_called()

    def test_lost_reset_ack_is_not_retried(self):
        with patch.object(c,"devices",side_effect=[{c.LOADER},{c.STOCK}]),patch.object(c,"variable",return_value="kona"),patch.object(c,"fastboot",side_effect=subprocess.TimeoutExpired("fastboot",8)) as fb:
            c.recover(False,True,3)
            fb.assert_called_once()

    def test_ambiguous_target_rejected(self):
        with patch.object(c,"devices",return_value={c.STOCK,c.LOADER}),patch.object(c,"fastboot") as fb:
            with self.assertRaisesRegex(c.RecoveryError,"ambiguous"):c.recover(True,False,3)
            fb.assert_not_called()

    def test_timeout_when_stock_never_enumerates(self):
        with patch.object(c,"devices",return_value={c.LOADER}),patch.object(c.time,"monotonic",side_effect=[0,0,3]),patch.object(c.time,"sleep"):
            with self.assertRaisesRegex(c.RecoveryError,"timed out"):c.await_target({c.STOCK},2)

if __name__=="__main__":unittest.main()
