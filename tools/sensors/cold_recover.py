"""Bounded IN2010 UEFI -> stock ABL FASTBOOT recovery with USB connected."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def stock_serial():
    """The phone's serial in the stock fastboot: HK_STOCK_SERIAL, or the one line of stock-serial.local beside this file (not in git)."""
    serial = os.environ.get("HK_STOCK_SERIAL", "").strip()
    local = Path(__file__).with_name("stock-serial.local")
    if not serial and local.exists():
        serial = local.read_text().strip()
    return serial or "stock-serial-not-set"


STOCK = stock_serial()
LOADER = "xnuloader"


class RecoveryError(RuntimeError):
    pass


def fastboot(*args, timeout=8):
    r = subprocess.run(["fastboot", *args], cwd=ROOT, capture_output=True,
                       text=True, timeout=timeout, check=False)
    output = (r.stdout + r.stderr).strip()
    if r.returncode:
        raise RecoveryError(output or f"fastboot exited {r.returncode}")
    return output


def devices():
    return {row[0] for line in fastboot("devices").splitlines()
            if len(row := line.split()) >= 2 and row[1] == "fastboot"}


def variable(serial, name):
    text = fastboot("-s", serial, "getvar", name)
    found = re.search(rf"(?m)^(?:\(bootloader\)\s*)?{re.escape(name)}:[ \t]*([^\r\n]*)$", text)
    return found.group(1).strip() if found else ""


def await_target(allowed, seconds, check=None):
    end = time.monotonic() + seconds
    last = "absent"
    while time.monotonic() < end:
        try:
            seen = devices() & {STOCK, LOADER}
        except (RecoveryError, subprocess.TimeoutExpired):
            seen = set()
        if seen == {STOCK, LOADER}:
            raise RecoveryError("both target serials present; ambiguous phone")
        if seen:
            serial = next(iter(seen))
            last = serial
            try:
                if serial in allowed and (check is None or check(serial)):
                    return serial
            except (RecoveryError, subprocess.TimeoutExpired):
                pass
        time.sleep(1)
    raise RecoveryError(f"timed out waiting for {','.join(sorted(allowed))}; last observed {last}")


def recover(developer, stock_only, seconds, witness=None):
    image = ROOT / "out" / ("instantnoodle-fwreader.img" if developer else "instantnoodle-uefi-fast.img")
    if not stock_only and not image.is_file():
        raise RecoveryError(f"missing boot image {image}")
    serial = await_target({STOCK, LOADER}, seconds)
    start = time.monotonic()
    if witness is not None:
        witness("source",serial)
    if serial == LOADER:
        if witness is not None:
            witness("reset-request",LOADER)
        print("UEFI: requesting stock ABL FASTBOOT; allowing the loader's cable timer to expire", flush=True)
        try:
            fastboot("-s", LOADER, "reboot", "bootloader")
        except (RecoveryError, subprocess.TimeoutExpired) as exc:
            if "remote:" in str(exc):
                raise
            print("USB disconnected during reset; checking for stock FASTBOOT", flush=True)
    await_target({STOCK}, seconds, lambda dev: variable(dev, "product") == "kona")
    if witness is not None:
        witness("stock",STOCK)
    print(f"verified stock FASTBOOT, product kona, after {time.monotonic()-start:.1f}s", flush=True)
    if stock_only:
        return
    if witness is not None:
        witness("developer-boot",STOCK)
    print(fastboot("-s", STOCK, "boot", str(image), timeout=45), flush=True)
    if developer:
        await_target({LOADER}, seconds, lambda dev: variable(dev, "result") == "idle")
        if witness is not None:
            witness("idle",LOADER)
        print("result: idle", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--developer", action="store_true")
    modes.add_argument("--stock-fastboot", action="store_true", help="stop at verified stock FASTBOOT")
    parser.add_argument("--timeout", type=int, default=120, help="seconds per enumeration phase")
    options = parser.parse_args()
    if not 1 <= options.timeout <= 1800:
        parser.error("timeout must be 1..1800 seconds")
    try:
        recover(options.developer, options.stock_fastboot, options.timeout)
    except (OSError, RecoveryError, subprocess.TimeoutExpired) as exc:
        print(f"cold recovery: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
