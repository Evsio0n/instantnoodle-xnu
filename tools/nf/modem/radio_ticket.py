"""Run the fixed IN2010 RAM radio observation with both resets owned by bootq."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/sensors"))
import cold_recover

@dataclass(frozen=True)
class Profile:
    name: str
    kernel_stem: str
    sha: str
    radio_level: int
    title: str

    @property
    def kernel(self):
        return ROOT / "out/kernels" / f"{self.kernel_stem}-{self.sha}.mach"


STATUS = Profile("radio", "nf-uim", "fd6741692d8be5718404381dfcbec1889e23be8e25687d74680e142f417647ac",
                 3, "IN2010 SIM presence and carrier registration")
FORMAT = Profile("data-format", "nf-format", "c19024b4bf422e96e8853bffb714c222043018ecbab0d1c4b140ae93a922ad73",
                 4, "IN2010 PCIe cellular link format")
CARD_CHECK = Profile("card-check", "nf-uim-reasons", "dba249f93f2849f51e67d6755460de3f11abe0a23e0e9dcdf323098fd65c8541",
                     5, "IN2010 card error state over time")
PHYSICAL = Profile("physical-sim", "nf-slot-ts", "6ba034c91e961dfbf3e28e185ccf9f8f6e5ee47656eb4d62d05ed1827d1c8c3f",
                   6, "IN2010 physical SIM slots and modem temperature")
ROOT_SHA = "5f545df7eed17ea798f3c62ad894e0f2f47227a1b1bd99b6247ed1eaa282eab9"
IMAGE = ROOT / f"out/radio-{ROOT_SHA}.apfs"
LIMIT = 140
ARGS = ("fb_panel=amb655uv01 cpus=8 bootcpu=7 gpu=0 wifi=0 adsp=0 bt=0 "
        "sound=0 sound_amps=0 haptic=0 cam=0 torch=0 syssleep=0 ufs=2 "
        "ufs_write=0 root_apfs=ufs_darwin hk_data=2 hk_data_mb=512 var_store=0 "
        "var_data=hfs var_data_format=1 modem=3 modem_mission=1 modem_radio={level} "
        "modem_loc=0 modem_gnss=0 battery_trace=1")

def check(profile=STATUS):
    for path, sha in ((profile.kernel, profile.sha), (IMAGE, ROOT_SHA)):
        with path.open("rb") as f:
            if hashlib.file_digest(f, "sha256").hexdigest() != sha:
                raise ValueError(f"artifact changed: {path.name}")
    prefix = (ROOT.parent / "oneplus8-audit/gpu-plan/observed-loader-prefix.txt").read_bytes()
    budget = len(prefix) + len(f"loader_return_after={LIMIT} epoch=0000000000 sb_delay=8 {ARGS.format(level=profile.radio_level)}")
    mac = ROOT / "fw-wifi/mac"
    if mac.is_file():
        budget += len(" wifi_mac=") + len(b"".join(mac.read_bytes().split()))
    if budget + 16 > 607:
        raise ValueError(f"{budget} command bytes leaves too little room")
    return budget

def data_format(line):
    match = re.fullmatch(r"modem: WDA format: endpoint=pcie/4 present=([0-9a-f]+) "
                         r"qos=(\d+) link=(\d+) uplink=(\d+) downlink=(\d+)", line)
    if not match:
        return None
    present = int(match[1], 16)
    if present & ~15:
        return None
    return tuple(int(match[i+2]) if present & (1 << i) else None for i in range(4))


def uim_readiness(line):
    match = re.fullmatch(r"modem: UIM readiness: sample=(\d+) gen=(\d+) validity_tlv=([01]) "
                         r"flags=(\d+) raw_ready=(\d+) present_ready=(\d+) confirmed_ready=(\d+)", line)
    if not match:
        return None
    sample, generation, present, flags, raw, eligible, confirmed = map(int, match.groups())
    if (not generation or flags > 255 or not (confirmed <= eligible <= raw <= 255 * 255)
            or (not present and flags) or (confirmed and not flags)):
        return None
    return sample, generation, present, flags, raw, eligible, confirmed


def observe(profile):
    ticket = os.environ.get("BOOTQ_TICKET", "")
    if not ticket.isdecimal():
        raise ValueError("device observation requires a bootq ticket")
    folder = ROOT / "out/eye" / f"{profile.name}-ticket-{ticket}"
    folder.mkdir(mode=0o700)
    cold_recover.recover(True, False, 120)
    started = time.time()
    env = dict({k:v for k,v in os.environ.items() if not k.startswith("XL_")}, PATH="/opt/homebrew/opt/coreutils/libexec/gnubin:" + os.environ["PATH"],
               XL_CMDLINE=ARGS.format(level=profile.radio_level), XL_KERNEL=str(profile.kernel), XL_RAMDISK=str(IMAGE),
               XL_SECS=str(LIMIT), XL_FOLLOW="4", XL_FOLLOW_KEEP="1")
    cmd = ". common/lib.sh; load_device instantnoodle; ./xl wait 1; ./xl ramdisk userland/flashboot.apfs; ./xl run"
    try:
        with (folder / "xl.txt").open("x") as output:
            result = subprocess.run(["bash", "-ec", cmd], cwd=ROOT, env=env,
                                    stdout=output, stderr=subprocess.STDOUT, timeout=LIMIT+185)
        transcript = (folder / "xl.txt").read_text(errors="replace")
        stamps = re.findall(r"^result: returned[^\n]*\[logs/(\d{6})-run\.log\]\s*$", transcript, re.M)
        if result.returncode or len(stamps) != 1:
            raise RuntimeError("missing confirmed kernel return, see ticket transcript")
        stamp = stamps[0]
        snapshots = set((ROOT / "logs").glob(f"{stamp}-run.log*"))
        records = set()
        serving = set()
        sim = set()
        formats = set()
        card_checks = set()
        qualified_cards = set()
        physical_headers, physical_rows, temperatures = set(), set(), set()
        gauge = set()
        teardown = False
        pins_down = False
        for path in snapshots:
            if path.stat().st_mtime < started:
                continue
            data = path.read_text(errors="replace")
            _, marker, current = data.rpartition("== KERNEL LOG (the kernel is still running) ==")
            if not marker:
                continue
            teardown |= "modem: level 3 is done" in current
            pins_down |= "modem: a second after the last reset: MDM2AP status 0, errfatal 0; AP2MDM status 0; PERST 0" in current
            for line in current.splitlines():
                if line.startswith("modem: UIM readiness:"):
                    qualified = uim_readiness(line.strip())
                    if qualified is not None:
                        qualified_cards.add(qualified)
                if profile == PHYSICAL:
                    physical_sample(line.strip(), physical_headers, physical_rows, temperatures)
                if profile == CARD_CHECK and line.startswith("modem: UIM check:"):
                    match = re.fullmatch(r"modem: UIM check: sample=([012]) generation=(\d+) time_ms=(\d+) "
                                         r"slots=(\d+) error_slots=(\d+) reason_mask=([a-f0-9]+) "
                                         r"unknown_errors=(\d+) ready_apps=(\d+)", line.strip())
                    if match:
                        card_checks.add(tuple(int(v, 16 if n == 5 else 10) for n, v in enumerate(match.groups())))
                if profile == FORMAT and line.startswith("modem: WDA format:"):
                    decoded = data_format(line.strip())
                    if decoded is not None:
                        formats.add(decoded)
                if line.startswith("hk-gauge v=1 ") and line.endswith(" end"):
                    gauge.add(line)
                at = line.find("radio-observe: phase=")
                if at >= 0:
                    records.add(line[at:].strip())
                match = re.search(r"modem: NAS serving: registration=(\d+) cs=(\d+) ps=(\d+) "
                                  r"interfaces=(\d+) mask=(0x[0-9a-f]+|0) unknown=(\d+) unrecognized=(\d+)$", line)
                if match:
                    serving.add(tuple(int(v, 0) for v in match.groups()))
                match = re.search(r"modem: UIM status: slots=(\d+) absent=(\d+) present=(\d+) "
                                  r"card_error=(\d+) other=(\d+) ready_apps=(\d+)$", line)
                if match:
                    sim.add(tuple(map(int, match.groups())))
        valid = any(re.fullmatch(r"radio-observe: phase=2 mode\.state=3 value=-?\d+ detail=-?\d+ signal\.state=3 value=-?\d+ detail=-?\d+ packet\.state=3 value=-?\d+ detail=-?\d+", line)
                    for line in records)
        gauge_log = folder / "gauge.txt"
        gauge_log.write_text("\n".join(sorted(gauge)) + "\n")
        analyzer = ROOT.parent / "oneplus8-audit/gauge-trace-next/analyze.py"
        gauge_result = subprocess.run([sys.executable, str(analyzer), str(gauge_log)],
                                      capture_output=True, text=True, timeout=20)
        energy = json.loads(gauge_result.stdout) if gauge_result.returncode == 0 else {}
        observed = (valid and len(serving) == 1 and len(sim) == 1 and teardown and pins_down
                    and energy.get("unique_samples", 0) >= 2)
        if profile == FORMAT:
            observed = (teardown and pins_down and len(formats) == 1
                        and all(v is not None for v in next(iter(formats))[1:]))
        if profile == CARD_CHECK:
            observed = teardown and pins_down and confirmed_card_checks(card_checks)
        if profile == PHYSICAL:
            observed = teardown and pins_down and confirmed_physical_slots(physical_headers, physical_rows)
        fields = ("registration", "cs_attach", "ps_attach", "interface_count", "mask", "unknown", "unrecognized")
        sim_fields = ("slots", "absent", "present", "card_error", "other", "ready_apps")
        receipt = dict(ticket=int(ticket), run=stamp, kernel=profile.sha, root=ROOT_SHA,
                       snapshots=sorted(records), all_three_values=valid, modem_quiesced=teardown and pins_down,
                       serving=[dict(zip(fields, values)) for values in sorted(serving)],
                       card_status=[dict(zip(sim_fields, values)) for values in sorted(sim)], gauge=energy)
        if qualified_cards:
            keys = ("sample", "generation", "validity_tlv", "validity_count", "raw_ready",
                    "present_ready", "confirmed_ready")
            receipt["uim_readiness"] = [dict(zip(keys, row)) for row in sorted(qualified_cards)]
        if profile == FORMAT:
            receipt["wda"] = [dict(zip(("qos", "link", "uplink", "downlink"), value)) for value in formats]
        if profile == CARD_CHECK:
            keys = ("sample","generation","time_ms","slots","error_slots","reason_mask","unknown_errors","ready_apps")
            receipt["card_checks"] = [dict(zip(keys, row)) for row in sorted(card_checks)]
            receipt["timed_checks_confirmed"] = observed
        if profile == PHYSICAL:
            keys = ('slot','card','activity','logical','protocol','atr_present','info_valid')
            receipt['physical_slots'] = [dict(zip(keys,row)) for row in sorted(physical_rows)]
            receipt['physical_status_confirmed'] = observed
            receipt['thermal_millicelsius'] = [list(item) for item in sorted(temperatures)]
        (folder / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if profile == FORMAT:
            print(f"run={stamp} PCIe WDA reports={len(formats)} complete link/uplink/downlink={observed}", flush=True)
        else:
            print(f"run={stamp} public snapshots={len(records)} all three measured={valid}", flush=True)
        return 0 if observed else 1
    finally:
        print("Restoring Developer mode before releasing radio ticket", flush=True)
        cold_recover.recover(True, False, 120)

def confirmed_card_checks(checks):
    rows = sorted(checks)
    return (len(rows) == 3 and [r[0] for r in rows] == [0, 1, 2]
            and len({r[1] for r in rows}) == 1 and rows[0][1] > 0
            and 0 < rows[0][2] < rows[1][2] < rows[2][2] and rows[2][2] - rows[0][2] >= 20000
            and all(0 <= errors <= slots <= 255 and 0 <= mask <= 255
                    and mask.bit_count() + unknown <= errors and bool(mask or unknown) == bool(errors)
                    for _, _, _, slots, errors, mask, unknown, _ in rows))


def physical_sample(line, headers, rows, temperatures):
    if line.startswith("modem: physical UIM:"):
        m = re.fullmatch(r"modem: physical UIM: slots=(\d+) info_present=([01])", line)
        if m:
            headers.add(tuple(map(int, m.groups())))
        else:
            m = re.fullmatch(r"modem: physical UIM: slot=(\d+) card=(\d+) activity=(\d+) logical=(\d+) "
                             r"protocol=(\d+) atr_present=([01]) info_valid=([01])", line)
            if m:
                rows.add(tuple(map(int, m.groups())))
    elif line.startswith("modem: TS current:"):
        m = re.fullmatch(r"modem: TS current: sensor=modem_tsens seq=(\d+) millicelsius=(-?\d+)", line)
        if m and int(m[1]) > 0 and -50000 <= int(m[2]) <= 150000:
            temperatures.add((int(m[1]), int(m[2])))


def confirmed_physical_slots(headers, rows):
    if len(headers) != 1:
        return False
    count, info_valid = next(iter(headers))
    return (1 <= count <= 2 and len(rows) == count and {r[0] for r in rows} == set(range(1, count + 1))
            and all(r[-1] == info_valid for r in rows))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    options = p.add_mutually_exclusive_group()
    options.add_argument("--run", action="store_true")
    options.add_argument("--inside-ticket", action="store_true")
    variant = p.add_mutually_exclusive_group()
    variant.add_argument("--data-format", action="store_true", help="query the format of the PCIe data endpoint")
    variant.add_argument("--card-check", action="store_true", help="observe card error enums at three points in time")
    variant.add_argument("--physical-sim", action="store_true", help="read physical slot state and modem temperature")
    args = p.parse_args()
    profile = STATUS
    if args.card_check:
        profile = CARD_CHECK
    elif args.data_format:
        profile = FORMAT
    elif args.physical_sim:
        profile = PHYSICAL
    print(f"radio: pinned kernel and RAM root verified, command {check(profile)}/607 bytes", flush=True)
    if args.inside_ticket:
        return observe(profile)
    if args.run:
        if os.environ.get("BOOTQ_TICKET"):
            raise ValueError("cannot nest tickets")
        worker = [sys.executable, str(Path(__file__).resolve()), "--inside-ticket"]
        if args.data_format:
            worker.append("--data-format")
        elif args.card_check:
            worker.append("--card-check")
        elif args.physical_sim:
            worker.append("--physical-sim")
        command = shlex.join(worker)
        return subprocess.run([str(ROOT / "tools/bootq"), "run", "--who", "radio", "--what",
                               profile.title, "--no-touch", "--max", "600",
                               "--", command], cwd=ROOT).returncode
    return 0

if __name__ == "__main__":
    rc = main()
    if rc == 0 and '--inside-ticket' in sys.argv and '--card-check' not in sys.argv:
        sys.path.insert(0, str(ROOT/'tools/metal/loop'))
        from receipt_matrix import finished
        profile_name = 'radio-observe'
        if '--physical-sim' in sys.argv:
            profile_name = 'sim-physical'
        elif '--data-format' in sys.argv:
            profile_name = 'cell-data-format'
        finished(profile_name)
    sys.exit(rc)
