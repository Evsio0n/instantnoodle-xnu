"""Host-only tests. Neither a USB tool nor a real bootq ticket is started."""

import contextlib
import importlib.util
import io
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("sensor_runner", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class SensorRunnerTests(unittest.TestCase):
    def setUp(self):
        self.stage = runner.build_plan("stage1")

    def test_default_checks_without_processes(self):
        with patch.object(runner.subprocess, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.main(["stage1"]), 0)
        run.assert_not_called()

    def test_missing_mandatory_disables_refused(self):
        for missing in runner.REQUIRED_OFF:
            with self.subTest(missing=missing), self.assertRaises(runner.PlanError):
                runner.validate_tokens("stage1", [t for t in self.stage.cmdline.split() if t != missing])

    def test_stage1_cannot_start_slpi(self):
        for extra in ("slpi_boot_once=1", "slpi_glink_probe=2", "slpi_beside_adsp=1"):
            with self.subTest(extra=extra), self.assertRaises(runner.PlanError):
                runner.validate_tokens("stage1", self.stage.cmdline.split() + [extra])

    def test_oversized_line_refused(self):
        with self.assertRaisesRegex(runner.PlanError, "bytes; limit is 607"):
            runner.build_plan("stage1", extra_tokens=["padding=" + "x" * 300])

    def test_missing_kernel_refused(self):
        with self.assertRaisesRegex(runner.PlanError, "kernel does not exist"):
            runner.build_plan("stage1", kernel=Path("/nonexistent/sensor-kernel"))

    def test_see_requires_provenance(self):
        for mode in ("see", "inventory"):
            with self.subTest(mode=mode), self.assertRaisesRegex(runner.PlanError, "cold-provenance"):
                runner.build_plan(mode)

    def test_inventory_keeps_exact_see_contract_except_probe(self):
        with tempfile.NamedTemporaryFile("w") as provenance:
            provenance.write("TEST FIXTURE ONLY; no real cold reset asserted\n")
            provenance.flush()
            see = runner.build_plan("see", cold_provenance=Path(provenance.name))
            inventory = runner.build_plan("inventory", cold_provenance=Path(provenance.name))
            self.assertEqual(inventory.cmdline, see.cmdline.replace("slpi_glink_probe=2", "slpi_glink_probe=4"))

    def test_see_budget_includes_private_mac_without_exposure(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            provenance, mac = directory / "cold.txt", directory / "mac"
            provenance.write_text("TEST FIXTURE ONLY; no real cold reset asserted\n")
            mac.write_text("02:00:00:00:00:01\n")
            plan = runner.build_plan("see", cold_provenance=provenance, mac_file=mac)
            self.assertEqual(plan.wifi_mac_bytes, 27)
            self.assertLessEqual(plan.final_bytes, 607)
            self.assertNotIn(mac.read_text().strip(), repr(plan))

    def test_ticket_wrapper_preserves_argv_and_cmdline(self):
        with tempfile.TemporaryDirectory(prefix="sensor test ") as name:
            directory = Path(name)
            captured = directory / "arguments"
            # The generated wrapper is executed through the same join/shell as
            # bootq, but its interpreter is an inert local argument recorder.
            interpreter = directory / "python ' capture.sh"
            interpreter.write_text("#!/bin/bash\nprintf '%s\\0' \"$@\" > " + shlex.quote(str(captured)) + "\n")
            interpreter.chmod(0o700)
            with patch.object(runner.sys, "executable", str(interpreter)):
                wrapper = runner._wrapper(directory / "queue ' entry.sh", self.stage)
            subprocess.run(["bash", "-c", " ".join([shlex.quote(str(wrapper))])], check=True)
            argv = captured.read_bytes().decode().rstrip("\0").split("\0")
            parsed = dict(zip(argv[4::2], argv[5::2]))
            self.assertEqual(argv[1:4], ["--inside-ticket", "--run", "stage1"])
            self.assertEqual(parsed["--done"], self.stage.done)
            self.assertEqual(parsed["--kernel"], str(self.stage.kernel))
            rebuilt = runner.build_plan("stage1", kernel=Path(parsed["--kernel"]),
                                        ramdisk=Path(parsed["--ramdisk"]), resident=Path(parsed["--resident"]),
                                        prefix=Path(parsed["--loader-prefix"]), seconds=int(parsed["--seconds"]))
            self.assertEqual(rebuilt.cmdline, self.stage.cmdline)

    def test_internal_execution_needs_ticket_before_usb(self):
        with patch.dict(runner.os.environ, {}, clear=True), patch.object(runner, "_wait_product") as wait:
            with self.assertRaisesRegex(RuntimeError, "active bootq ticket"):
                runner._execute(self.stage)
            wait.assert_not_called()


if __name__ == "__main__":
    unittest.main()
