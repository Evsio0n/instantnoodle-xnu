"""Queue-owned sensor run and USB recovery for the owner's test page."""
import hashlib
import json
import os
import plistlib
import re
from pathlib import Path
import subprocess
import sys
import time

import cold_recover
import run as sensor
import record_matrix

ROOT = sensor.ROOT
RAMDISK = ROOT / "out/slpi-broker-cal.apfs"
KERNEL_SHA = "b4c3ce76df860cd736691f9fe6e02e5bbfddc24dfa23bbea890e721706c1917c"
ROOT_SHA = "1568a4046bc1b2e4a98783315941c52a0a85c9c48d2b4f17943c8e67223a94ab"

def verify_image(path, expected):
    with path.open("rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != expected:
            raise ValueError(f"sensor artifact changed: {path.name}")


def usb_session():
    """macOS assigns a new sessionID when the USB device re-enumerates."""
    def visit(node):
        if isinstance(node,list):
            for child in node: yield from visit(child)
        elif isinstance(node,dict):
            if node.get("USB Serial Number")==cold_recover.LOADER:
                yield {key:node.get(key) for key in ("sessionID","IORegistryEntryID","locationID")}
            yield from visit(node.get("IORegistryEntryChildren",[]))
    try:
        raw=subprocess.check_output(["ioreg","-a","-p","IOUSB","-l"],timeout=5)
        matches=list(visit(plistlib.loads(raw)))
        if len(matches)==1 and all(isinstance(v,int) and v>0 for v in matches[0].values()):
            return matches[0]
    except (OSError,subprocess.SubprocessError,ValueError):
        pass
    return None


def reusable_reset(ticket):
    previous=ticket-1
    receipt=ROOT/"out/eye"/f"sensor-ticket-{previous}"/"end-cold.json"
    try:
        data=json.loads(receipt.read_text())
        if data["ticket"]!=previous or not 0<=time.time()-data["epoch"]<=900:
            return None
        history=subprocess.check_output([str(ROOT/"tools/bootq"),"log","1"],text=True,timeout=5)
        if not re.search(rf"#{previous} sensor: SLPI registry bring-up.*ended, rc 0,",history):
            return None
        session=usb_session()
        if session and data.get("session")==session and cold_recover.variable(cold_recover.LOADER,"result")=="idle":
            return data
    except (KeyError,ValueError,OSError,subprocess.SubprocessError,cold_recover.RecoveryError):
        pass
    return None


def main(*, kernel=None, extra_tokens=(), ramdisk=RAMDISK, ramdisk_sha=ROOT_SHA):
    ticket = os.getenv("BOOTQ_TICKET", "")
    if not ticket.isdecimal():
        raise RuntimeError("an active bootq ticket is required")
    if kernel is None:
        kernel = ROOT / "out/kernels" / f"nf-als-deps-{KERNEL_SHA}.mach"
        verify_image(kernel, KERNEL_SHA)
    verify_image(ramdisk, ramdisk_sha)
    folder = ROOT / "out/eye" / f"sensor-ticket-{ticket}"
    folder.mkdir(mode=0o700, exist_ok=False)
    provenance = folder / "cold.json"
    # A developer USB boot from stock fastboot by itself is not evidence
    # about how ABL was entered. Start the diagnostic from a witnessed reset.
    previous=reusable_reset(int(ticket))
    if previous:
        print(f"Reusing confirmed post-ticket {int(ticket)-1} cold boot; USB session and idle result match",flush=True)
        evidence={"ticket":int(ticket),"from_previous":previous}
    else:
        if cold_recover.LOADER not in cold_recover.devices():
            cold_recover.recover(True,False,120)
        cold_recover.recover(True,False,120)
        evidence={"ticket":int(ticket),"epoch":time.time(),"observed":[
            "UEFI reboot bootloader","stock serial/product kona","developer RAM boot","xnuloader idle"]}
    provenance.write_text(json.dumps(evidence)+"\n")
    plan = sensor.build_plan("rpc-std", kernel=kernel, ramdisk=ramdisk, cold_provenance=provenance, extra_tokens=extra_tokens)
    print("Starting bounded RAM sensor registry probe", flush=True)
    try:
        before = (ROOT/"logs/last.log").resolve()
        rc = sensor._execute(plan)
        returned = (ROOT/"logs/last.log").resolve()
        live = returned.with_name(returned.name + ".live")
        if not rc and returned!=before and live.is_file():
            record_matrix.record(int(ticket), live)
        return rc
    finally:
        print("Restoring clean developer mode before releasing ticket", flush=True)
        cold_recover.recover(True, False, 120)
        session=usb_session()
        if session:
            record={"ticket":int(ticket),"epoch":time.time(),"session":session}
            temp=folder/"end-cold.tmp"
            temp.write_text(json.dumps(record)+"\n")
            temp.replace(folder/"end-cold.json")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(f"sensor ticket: {error}")
