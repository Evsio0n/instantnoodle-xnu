#!/usr/bin/env python3
"""Add the SLPI firmware and preference bridge to a cellular RAM root image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
CONTAINER = "oneplus8-build"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "out/cell-als-root.apfs")
    parser.add_argument("--calibration-reader", type=Path,
                        help="Override out/cell-als-reader.txt from build_reader.py --verified-current")
    parser.add_argument("--calibration-receipt", type=Path,
                        default=ROOT / "out/als-calibration-249.sha256")
    parser.add_argument("--enable-once-client", type=Path,
                        help="Add an explicit RAM-only system preference enable test")
    args = parser.parse_args()
    base = args.base.resolve()
    output = args.output.resolve()
    if output == base:
        parser.error("output must differ from the source root image")
    assets = ROOT / "out/slpi-broker-root"
    firmware = sorted((assets / "fw").glob("slpi*"))
    reader_pin = ROOT / "out/cell-als-reader.txt"
    if args.calibration_reader is None:
        if not reader_pin.is_file():
            raise RuntimeError("run build_reader.py --verified-current before packaging the root")
        args.calibration_reader = Path(reader_pin.read_text().strip())
    calibration_reader = args.calibration_reader.resolve()
    if not firmware or not (assets / "fw/slpi-registry.pack").is_file():
        raise RuntimeError("SLPI firmware and registry assets are missing")
    if not calibration_reader.is_file():
        raise RuntimeError("the factory-calibration reader is missing")
    remote = subprocess.check_output(
        ["docker", "exec", CONTAINER, "mktemp", "-d", "/tmp/cell-als-root.XXXXXXXX"],
        text=True,
    ).strip()
    try:
        subprocess.run(["docker", "cp", str(HERE / "pref_bridge.m"),
                        f"{CONTAINER}:{remote}/pref_bridge.m"], check=True)
        command = (
            "source /build/xnu-env.sh\n"
            "xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk "
            "-fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation "
            "-framework CoreFoundation pref_bridge.m -o als_prefbridge"
        )
        subprocess.run(["docker", "exec", "-e", "THEOS=/build/local-theos", "-w", remote,
                        CONTAINER, "bash", "-ec", command], check=True)
        with tempfile.TemporaryDirectory(prefix="als-root-", dir=ROOT / "out") as temp:
            stage = Path(temp) / "root"
            subprocess.run([sys.executable, str(HERE.parent / "unpack_root.py"),
                            str(base), str(stage)], check=True)
            # unpack_root deliberately skips the Wi-Fi file. Restore exactly the
            # source image's entry, without printing it or substituting host state.
            sys.path.insert(0, str(ROOT / "tools/apfs"))
            import apfs
            container = apfs.Container(str(base))
            try:
                volume = apfs.pick_volume(container, None)
                entry = volume.lookup("/fw/network")
                if entry is not None:
                    network = stage / "fw/network"
                    network.write_bytes(volume.inode(entry[0]).contents())
                    network.chmod(0o600)
            finally:
                container.close()
            for source in firmware:
                shutil.copy2(source, stage / "fw" / source.name)
            shutil.copy2(calibration_reader, stage / "bin/read_cali_coe")
            (stage / "bin/read_cali_coe").chmod(0o755)
            if args.calibration_receipt:
                receipt_bytes = args.calibration_receipt.read_bytes()
                if len(receipt_bytes) != 32:
                    raise ValueError("calibration receipt must contain a 32-byte SHA-256 digest")
                receipt_file = stage / "fw/als-calibration-249.sha256"
                receipt_file.write_bytes(receipt_bytes)
                receipt_file.chmod(0o600)
            bridge = stage / "bin/als_prefbridge"
            subprocess.run(["docker", "cp", f"{CONTAINER}:{remote}/als_prefbridge",
                            str(bridge)], check=True)
            bridge.chmod(0o755)
            jobs = stage / "etc/rc.jobs"
            original_jobs = jobs.read_text()
            added_jobs = "/hackios/bin/read_cali_coe\n/hackios/bin/als_prefbridge\n"
            if args.enable_once_client:
                enable_client = stage / "bin/als_enable_once"
                shutil.copy2(args.enable_once_client, enable_client)
                enable_client.chmod(0o755)
                added_jobs += "/hackios/bin/als_enable_once --enable-once\n"
            jobs.write_text(added_jobs + original_jobs)
            built = Path(temp) / "root.apfs"
            subprocess.run([sys.executable, "tools/apfs/mkapfs.py", "-z", "--plain", "fw",
                            "-o", str(built), "hackios=" + str(stage)], cwd=ROOT, check=True)
            output.parent.mkdir(parents=True, exist_ok=True)
            built.chmod(0o600)
            built.replace(output)
            receipt = {
                "base": str(base),
                "base_sha256": hashlib.sha256(base.read_bytes()).hexdigest(),
                "root": str(output),
                "root_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "firmware_files": len(firmware),
                "calibration_reader_sha256": hashlib.sha256(calibration_reader.read_bytes()).hexdigest(),
                "bridge_source_sha256": hashlib.sha256((HERE / "pref_bridge.m").read_bytes()).hexdigest(),
                "base_jobs_preserved": jobs.read_text() == added_jobs + original_jobs,
                "enable_once_test": args.enable_once_client is not None,
                "enable_once_client_sha256": (
                    hashlib.sha256(args.enable_once_client.read_bytes()).hexdigest()
                    if args.enable_once_client else None
                ),
                "calibration_receipt_sha256": (
                    hashlib.sha256(args.calibration_receipt.read_bytes()).hexdigest()
                    if args.calibration_receipt else None
                ),
            }
            output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
            print(json.dumps(receipt, indent=2))
    finally:
        subprocess.run(["docker", "exec", CONTAINER, "rm", "-rf", remote], check=True)


if __name__ == "__main__":
    main()
