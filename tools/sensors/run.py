#!/usr/bin/env python3
"""Plan or queue RAM-only SLPI sensor probes from the host.

The normal invocation only validates a plan.  A phone is touched only when
``--run`` is supplied; the run then enters ``tools/bootq`` with a generated
wrapper so that shell quoting cannot alter the command line.
Stage1 observes SMEM only. SEE/inventory start SLPI once, query it, and stop the
owned DSP. Its explicit cold-provenance file records the operator's evidence;
neither that file nor an empty SMEM inventory proves a hardware cold reset.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
from typing import Iterable, Optional


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_KERNEL = ROOT / "xnu-src/nf-tree/xnu-8796.121.2/BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
DEFAULT_RAMDISK = ROOT / "out/slpi-boot-root.apfs"
RESIDENT_RAMDISK = ROOT / "userland/flashboot.apfs"
DEFAULT_PREFIX = ROOT.parent / "oneplus8-audit/gpu-plan/observed-loader-prefix.txt"
DEFAULT_MAC = ROOT / "fw-wifi/mac"
MAX_BOOT_LINE = 607                 # loader's 608-byte array includes the NUL
DEFAULT_STAGE_SECS = 120
DEFAULT_SEE_SECS = 150

REQUIRED_OFF = {
    "wifi=0", "adsp=0", "sound=0", "sound_amps=0", "cam=0", "bt=0",
    "haptic=0", "torch=0", "modem=0", "syssleep=0", "ufs_write=0",
    "var_store=0", "hk_data=2", "gpu=0", "fb_gpu=0",
}
FORBIDDEN = {
    "touchscreen=1", "wifi_again=1", "adsp_again=1", "ufs_write=1",
    "var_store=1", "slpi_boot_once=0",
}


class PlanError(ValueError):
    """A plan is unsafe or cannot fit in the loader's command-line buffer."""


@dataclass(frozen=True)
class Plan:
    mode: str
    kernel: Path
    ramdisk: Path
    resident: Path
    cmdline: str
    loader_prefix: str
    final_bytes: int
    seconds: int
    done: str
    cold_provenance: Optional[Path]
    wifi_mac_bytes: int
    prefix_path: Path
    mac_file: Path


def _token_map(tokens: Iterable[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for token in tokens:
        key = token.split("=", 1)[0]
        if key in out:
            raise PlanError(f"duplicate boot argument: {key}")
        out[key] = token
    return out


def validate_tokens(mode: str, tokens: Iterable[str]) -> None:
    tokens = list(tokens)
    if any(not t or any(c.isspace() or ord(c) < 32 or ord(c) > 126 for c in t) for t in tokens):
        raise PlanError("boot arguments must be individual ASCII tokens")
    mapping = _token_map(tokens)
    missing = sorted(REQUIRED_OFF - set(tokens))
    if missing:
        raise PlanError("mandatory safety arguments missing: " + ", ".join(missing))
    if FORBIDDEN.intersection(tokens):
        raise PlanError("forbidden boot arguments")
    probe = {"inventory": 4, "rpc-channel": 5, "rpc-intent": 6, "rpc-attach": 7, "rpc-listener": 8, "rpc-next": 9, "rpc-module": 10, "rpc-module-data": 11, "rpc-std": 12}.get(mode, 2)
    read_only = {
        "stage1": {"slpi_step=1", "slpi_smem_ro=1"},
        "smmu-ro": {"slpi_smmu_ro=1"},
        "dma-arena": {"slpi_dma_arena=1"},
        "bank-ro": {"slpi_bank_ro=1"},
        "dma-route": {"slpi_dma_arena=2", "slpi_cold_provenance=1"},
        "dma-context": {"slpi_dma_arena=3", "slpi_cold_provenance=1"},
    }
    expected = read_only.get(mode, {"slpi_boot_once=1", "slpi_cold_provenance=1",
                                    "slpi_hold_proxy=1", f"slpi_glink_probe={probe}"})
    if mode=="rpc-std" and mapping.get("slpi_oem_board")=="slpi_oem_board=1":
        expected=expected | {"slpi_oem_board=1"}
        attrs=mapping.get("slpi_oem_attrs")
        if attrs in ("slpi_oem_attrs=1","slpi_oem_attrs=2","slpi_oem_attrs=3"):
            expected=expected | {attrs}
    if mode=="rpc-std" and mapping.get("slpi_oem_board")=="slpi_oem_board=2":
        expected=expected | {"slpi_oem_board=2"}  # Private IMU-only profile.
        if mapping.get("slpi_imu") in ("slpi_imu=1","slpi_imu=2"):
            expected=expected | {mapping["slpi_imu"]}
    actual = {t for t in tokens if t.startswith("slpi_")}
    if actual != expected:
        raise PlanError(f"{mode} requires its exact SLPI contract; other SLPI flags are refused")
    for key in ("wifi_again", "adsp_again", "adsp_step", "battery", "loader_return_after", "epoch", "sb_delay", "wifi_mac"):
        if key in mapping:
            raise PlanError(f"runner controls or forbids boot argument: {key}")


def _read_prefix(path: Path) -> str:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise PlanError(f"loader prefix is unavailable: {path}") from exc
    if not raw or b"\x00" in raw or b"\n" in raw or b"\r" in raw:
        raise PlanError("loader prefix must be one NUL-free line without a newline")
    try:
        value = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise PlanError("loader prefix is not ASCII") from exc
    if value.count("ufs_shared_pa=0x") != 1:
        raise PlanError("loader prefix must contain exactly one ufs_shared_pa=0x")
    fields = _token_map(value.split())
    if fields.get("board") != "board=instantnoodle" or fields.get("rd") != "rd=md0":
        raise PlanError("loader prefix must identify instantnoodle and resident rd=md0")
    if not re.fullmatch(r"ufs_shared_pa=0x[0-9a-fA-F]{16}", fields["ufs_shared_pa"]):
        raise PlanError("loader prefix must include the full 16-digit UFS shared address")
    if any(key.startswith("slpi_") or key in {t.split('=')[0] for t in REQUIRED_OFF} for key in fields):
        raise PlanError("loader prefix contains runner-owned boot arguments")
    # loader.c terminates every generated prefix field with a space.  Keep that
    # delimiter even when a copied observation omitted its final byte.
    if not value.endswith(" "):
        value += " "
    return value


def _wifi_mac_extra(path: Path, tokens: Iterable[str]) -> tuple[str, int]:
    """Mirror xl's private wifi_mac append without ever printing the address."""
    if not any(t == "wifi=0" or t.startswith("wifi=") or t.startswith("wifi_step=") for t in tokens):
        return "", 0
    try:
        raw = path.read_text().strip()
    except FileNotFoundError:
        return "", 0                       # xl's ``[ -s ]`` does the same
    except OSError as exc:
        raise PlanError(f"cannot read the private Wi-Fi address file: {path}") from exc
    if not raw:
        return "", 0
    if not re.fullmatch(r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}", raw):
        raise PlanError("private Wi-Fi address file is malformed")
    extra = "wifi_mac=" + raw
    return extra, len(extra.encode("ascii")) + 1  # xl adds one separating space


def _check_files(kernel: Path, ramdisk: Path, resident: Path) -> None:
    for label, path in (("kernel", kernel), ("probe ramdisk", ramdisk), ("resident ramdisk", resident)):
        if not path.is_file():
            raise PlanError(f"{label} does not exist: {path}")
        if path.stat().st_size == 0:
            raise PlanError(f"{label} is empty: {path}")
    # xl currently expands kernel= and ramdisk= without shell quoting.
    for path in (kernel, ramdisk):
        if not re.fullmatch(r"[A-Za-z0-9_./+@=-]+", str(path)):
            raise PlanError("xl requires kernel and probe ramdisk paths with shell-safe names")


def build_plan(
    mode: str,
    *,
    kernel: Path = DEFAULT_KERNEL,
    ramdisk: Path = DEFAULT_RAMDISK,
    resident: Path = RESIDENT_RAMDISK,
    prefix: Path = DEFAULT_PREFIX,
    mac_file: Path = DEFAULT_MAC,
    cold_provenance: Optional[Path] = None,
    seconds: Optional[int] = None,
    extra_tokens: Iterable[str] = (),
    check_files: bool = True,
) -> Plan:
    if mode not in ("stage1", "see", "inventory", "rpc-channel", "rpc-intent", "smmu-ro", "dma-arena", "bank-ro", "dma-route", "dma-context", "rpc-attach", "rpc-listener", "rpc-next", "rpc-module", "rpc-module-data", "rpc-std"):
        raise PlanError("mode must be stage1, see or inventory")
    kernel, ramdisk, resident, prefix, mac_file = (Path(p).resolve() for p in (kernel, ramdisk, resident, prefix, mac_file))
    if check_files:
        _check_files(kernel, ramdisk, resident)
    provenance: Optional[Path] = None
    if mode not in ("stage1", "smmu-ro", "dma-arena", "bank-ro"):
        if cold_provenance is None:
            raise PlanError("SEE/inventory require an explicit --cold-provenance file")
        provenance = Path(cold_provenance).resolve()
        if check_files and (not provenance.is_file() or provenance.stat().st_size == 0):
            raise PlanError(f"cold provenance file is missing or empty: {provenance}")
    elif cold_provenance is not None:
        raise PlanError("--cold-provenance is only valid for SEE/inventory")

    tokens = [
        "fb_panel=amb655uv01", "fb_hz=90", "cpus=8", "bootcpu=7",
        "gpu=0", "fb_gpu=0",
        "syssleep=0", "ufs=2", "root_apfs=ufs_darwin", "hk_data=2", "hk_data_mb=512",
        "var_data=hfs", "var_data_format=1", "ufs_write=0", "var_store=0",
        "wifi=0", "adsp=0", "sound=0", "sound_amps=0", "cam=0", "bt=0",
        "haptic=0", "torch=0", "modem=0",
    ]
    if mode in ("dma-route", "dma-context"):
        tokens += ["slpi_dma_arena=2" if mode == "dma-route" else "slpi_dma_arena=3", "slpi_cold_provenance=1"]
        done = r"slpi-route: complete"
    elif mode == "bank-ro":
        tokens += ["slpi_bank_ro=1"]
        done = r"slpi-bank: complete"
    elif mode == "dma-arena":
        tokens += ["slpi_dma_arena=1"]
        done = r"slpi-arena: complete"
    elif mode == "smmu-ro":
        tokens += ["slpi_smmu_ro=1"]
        done = r"slpi-smmu: complete"
    elif mode == "stage1":
        tokens += ["slpi_step=1", "slpi_smem_ro=1"]
        done = r"slpi-ro: stage1 ended"
    else:
        probe = {"inventory": 4, "rpc-channel": 5, "rpc-intent": 6, "rpc-attach": 7, "rpc-listener": 8, "rpc-next": 9, "rpc-module": 10, "rpc-module-data": 11, "rpc-std": 12}.get(mode, 2)
        tokens += ["slpi_boot_once=1", "slpi_cold_provenance=1", "slpi_hold_proxy=1", f"slpi_glink_probe={probe}"]
        done = r"slpi: ended; active="
    tokens += list(extra_tokens)
    validate_tokens(mode, tokens)

    line = " ".join(tokens)
    seconds = int(seconds if seconds is not None else (DEFAULT_STAGE_SECS if mode == "stage1" else DEFAULT_SEE_SECS))
    if not 1 <= seconds <= 600:
        raise PlanError("seconds must be in 1..600")
    epoch = str(int(time.time()))
    controls = f"loader_return_after={seconds} epoch={epoch} sb_delay=8"
    wifi_extra, wifi_bytes = _wifi_mac_extra(Path(mac_file), tokens)
    loader_prefix = _read_prefix(prefix)
    final = loader_prefix + controls + " " + line
    if wifi_extra:
        final += " " + wifi_extra
    final_bytes = len(final.encode("ascii"))
    if final_bytes > MAX_BOOT_LINE:
        raise PlanError(f"loader command line is {final_bytes} bytes; limit is {MAX_BOOT_LINE}")
    return Plan(mode, kernel, ramdisk, resident, line, loader_prefix, final_bytes,
                seconds, done, provenance, wifi_bytes, prefix, mac_file)


def _wrapper(path: Path, plan: Plan) -> Path:
    """Create a path-only bootq command; keep it until bootq's child exits."""
    path.parent.mkdir(parents=True, exist_ok=True)
    args = [sys.executable, str(Path(__file__).resolve()), "--inside-ticket", "--run", plan.mode,
            "--kernel", str(plan.kernel), "--ramdisk", str(plan.ramdisk),
            "--resident", str(plan.resident), "--seconds", str(plan.seconds),
            "--loader-prefix", str(plan.prefix_path),
            "--done", plan.done]
    if plan.cold_provenance is not None:
        args += ["--cold-provenance", str(plan.cold_provenance)]
    # Arguments are fixed in this generated file and shell-quoted individually.  In
    # particular, a provenance filename containing $(...) is data, never shell code.
    text = "#!/bin/bash\nset -e\nexec " + " ".join(shlex.quote(a) for a in args) + "\n"
    path.write_text(text)
    path.chmod(0o700)
    return path


def _fastboot_product(timeout: float = 8.0) -> Optional[str]:
    try:
        devices = subprocess.run(["fastboot", "devices"], capture_output=True, text=True,
                                 timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if not any(line.strip().endswith("fastboot") for line in devices.stdout.splitlines()):
        return None
    try:
        got = subprocess.run(["fastboot", "getvar", "product"], capture_output=True, text=True,
                             timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r"(?:^|\n)\s*(?:\(bootloader\)\s*)?product:\s*([^\s\r\n]+)", got.stdout + got.stderr)
    return match.group(1) if match else "unknown"


def _wait_product(seconds: int = 120) -> str:
    """Bounded polling; use repeated device enumeration, never the unsupported wait subcommand."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        product = _fastboot_product()
        if product:
            return product
        time.sleep(2)
    raise RuntimeError(f"no fastboot device appeared within {seconds}s")


def _execute(plan: Plan) -> int:
    if not os.environ.get("BOOTQ_TICKET"):
        raise RuntimeError("execution requires an active bootq ticket")
    product = _wait_product()
    print(f"sensor: fastboot product={product}", flush=True)
    if product != "xnuloader":
        raise RuntimeError(f"phone is in {product} stock fastboot; loader recovery is left to the owner")
    env = {key: value for key, value in os.environ.items() if key not in ("XL_FORCE", "XL_TRANSACTION")}
    env.update(XL_KERNEL=str(plan.kernel), XL_RAMDISK=str(plan.ramdisk), XL_SECS=str(plan.seconds),
               XL_FOLLOW="2", XL_FOLLOW_KEEP="1", XL_DONE=plan.done, XL_CMDLINE=plan.cmdline, XL_SB_DELAY="8")
    coreutils = "/opt/homebrew/opt/coreutils/libexec/gnubin"
    env["PATH"] = coreutils + os.pathsep + env.get("PATH", "")
    command = ". common/lib.sh; load_device instantnoodle; ./xl ramdisk " + shlex.quote(str(plan.resident)) + "; ./xl run"
    return subprocess.run(["bash", "-ec", command], cwd=ROOT, env=env, check=False).returncode


def _self_tests() -> None:
    prefix = DEFAULT_PREFIX
    stage = build_plan("stage1", check_files=True)
    assert "slpi_boot_once" not in stage.cmdline
    assert REQUIRED_OFF.issubset(set(stage.cmdline.split()))
    for token in REQUIRED_OFF:
        try:
            validate_tokens("stage1", [t for t in stage.cmdline.split() if t != token])
        except PlanError:
            pass
        else:
            raise AssertionError(f"missing mandatory token accepted: {token}")
    try:
        validate_tokens("stage1", stage.cmdline.split() + ["slpi_boot_once=1"])
    except PlanError:
        pass
    else:
        raise AssertionError("stage1 accepted a DSP start")
    with tempfile.TemporaryDirectory() as missing_dir:
        try:
            build_plan("stage1", prefix=Path(missing_dir) / "missing-prefix", check_files=False)
        except PlanError:
            pass
        else:
            raise AssertionError("missing prefix accepted")
    try:
        build_plan("stage1", prefix=prefix, extra_tokens=["x" * 500], check_files=True)
    except PlanError as exc:
        assert "bytes" in str(exc)
    else:
        raise AssertionError("oversized command line accepted")
    try:
        build_plan("see", cold_provenance=None)
    except PlanError as exc:
        assert "cold-provenance" in str(exc)
    else:
        raise AssertionError("SEE bypassed cold provenance")
    # Shell round-trip for a hostile filename: this is the same quoting used by
    # the generated bootq wrapper and does not access USB.
    value = "/tmp/a b;$(touch SHOULD_NOT_EXIST)"
    roundtrip = subprocess.run(["bash", "-c", "printf '%s' " + shlex.quote(value)],
                               capture_output=True, text=True, check=True).stdout
    assert roundtrip == value
    print(f"PASS sensor host checks; stage1 loader budget={stage.final_bytes}/{MAX_BOOT_LINE}; no USB used")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("stage1", "see", "inventory", "rpc-channel", "rpc-intent", "smmu-ro", "dma-arena", "bank-ro", "dma-route", "dma-context", "rpc-attach", "rpc-listener", "rpc-next", "rpc-module", "rpc-module-data", "rpc-std"))
    parser.add_argument("--run", action="store_true", help="queue and execute the plan on the phone")
    parser.add_argument("--check", action="store_true", help="run host-only self-checks")
    parser.add_argument("--inside-ticket", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--kernel", type=Path, default=DEFAULT_KERNEL)
    parser.add_argument("--ramdisk", type=Path, default=DEFAULT_RAMDISK)
    parser.add_argument("--resident", type=Path, default=RESIDENT_RAMDISK)
    parser.add_argument("--loader-prefix", type=Path, default=DEFAULT_PREFIX)
    parser.add_argument("--cold-provenance", type=Path)
    parser.add_argument("--seconds", type=int)
    parser.add_argument("--done", default=None, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.check:
        _self_tests()
        return 0
    if not args.mode:
        parser.error("mode is required unless --check is used")
    try:
        plan = build_plan(args.mode, kernel=args.kernel, ramdisk=args.ramdisk, resident=args.resident,
                          prefix=args.loader_prefix,
                          cold_provenance=args.cold_provenance, seconds=args.seconds,
                          check_files=True)
    except PlanError as exc:
        print(f"sensor: refused: {exc}", file=sys.stderr)
        return 2
    if args.inside_ticket:
        if not args.run:
            # Hidden mode is only reached by the generated wrapper.  Keep the
            # guard explicit so it cannot accidentally become a second dry-run.
            print("sensor: internal execution requires --run", file=sys.stderr)
            return 2
        if args.done is not None:
            plan = Plan(plan.mode, plan.kernel, plan.ramdisk, plan.resident, plan.cmdline,
                        plan.loader_prefix, plan.final_bytes, plan.seconds, args.done,
                        plan.cold_provenance, plan.wifi_mac_bytes, plan.prefix_path, plan.mac_file)
        try:
            return _execute(plan)
        except (RuntimeError, OSError) as exc:
            print(f"sensor: {exc}", file=sys.stderr)
            return 1
    if not args.run:
        print(f"sensor: {plan.mode} plan valid; loader budget={plan.final_bytes}/{MAX_BOOT_LINE}; use --run to queue hardware")
        return 0
    wrapper_dir = Path(tempfile.mkdtemp(prefix="sensor-run-", dir=HERE))
    wrapper = _wrapper(wrapper_dir / "run.sh", plan)
    try:
        cmd = [str(ROOT / "tools/bootq"), "run", "--who", "sensor", "--what",
               f"SLPI {plan.mode} probe", "--no-touch", "--max", str(plan.seconds + 60), "--",
               shlex.quote(str(wrapper))]
        return subprocess.run(cmd, cwd=ROOT, check=False).returncode
    finally:
        try:
            wrapper.unlink()
            wrapper_dir.rmdir()
        except OSError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
