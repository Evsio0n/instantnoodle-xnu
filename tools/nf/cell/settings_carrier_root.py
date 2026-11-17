#!/usr/bin/env python3
"""Update the telephony daemon and optionally add a Preferences library to a verified RAM image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
KERNEL = HERE.parents[2]
sys.path.insert(0, str(HERE / "als"))
from settings_root import contents, sha

DAEMON = "bin/hk_commcenter"
PREFERENCES_LIBRARY = "lib/cellular-settings.dylib"
TWEAKS_LIBRARY = "lib/libhackios-tweaks.dylib"
INJECT_CONF = "etc/inject.conf"
PREFERENCES_RULE = b"/hackios/lib/cellular-settings.dylib Preferences"


def append_preferences_rule(config):
    if b"\0" in config:
        raise ValueError("Unexpected NUL in inject.conf")
    if any(rule.split()[:1] == [PREFERENCES_RULE.split()[0]]
           for rule in config.replace(b";", b"\n").splitlines()):
        raise ValueError("inject.conf already names the standalone Preferences library")
    separator = b"" if not config or config.endswith(b"\n") else b"\n"
    updated = config + separator + PREFERENCES_RULE + b"\n"
    # init trims trailing LF/space, converts LF to ';', then sends the NUL too.
    # sysctl_io_string rejects newlen >= sizeof(exec_inject_conf), which is 512.
    sysctl_bytes = len(updated.rstrip(b"\n ").replace(b"\n", b";")) + 1
    if sysctl_bytes > 511:
        raise ValueError(f"kern.exec_inject requires {sysctl_bytes} bytes including NUL (limit 511)")
    return updated, sysctl_bytes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--preferences-library", type=Path, metavar="FILE",
                        help="add the standalone cellular-settings library to Preferences")
    args = parser.parse_args()
    os.umask(0o077)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temp:
        stage = Path(temp) / "root"
        stage.mkdir()
        original = contents(args.base, stage)
        expected = dict(original)
        replaced = [DAEMON] if args.preferences_library is None else [DAEMON, INJECT_CONF]
        required = replaced if args.preferences_library is None else replaced + [TWEAKS_LIBRARY]
        for name in required:
            if name not in original or not stat.S_ISREG(original[name][0]):
                raise ValueError(f"The input image has no regular file at {name}")
        payload = args.daemon.read_bytes()
        (stage / DAEMON).write_bytes(payload)
        expected[DAEMON] = (*original[DAEMON][:3], hashlib.sha256(payload).hexdigest())
        if args.preferences_library is not None:
            if PREFERENCES_LIBRARY in original:
                raise ValueError(f"The input image already contains {PREFERENCES_LIBRARY}")
            config = stage / INJECT_CONF
            prefix = config.read_bytes()
            updated, sysctl_bytes = append_preferences_rule(prefix)
            if hashlib.sha256(updated[:len(prefix)]).hexdigest() != original[INJECT_CONF][3]:
                raise ValueError("The inject.conf prefix differs from the input APFS image")
            config.write_bytes(updated)
            expected[INJECT_CONF] = (*original[INJECT_CONF][:3], hashlib.sha256(updated).hexdigest())
            library = stage / PREFERENCES_LIBRARY
            library.write_bytes(args.preferences_library.read_bytes())
            library.chmod(0o755)
            expected[PREFERENCES_LIBRARY] = (stat.S_IFREG | 0o755, 0, 0, sha(library))
        stage.chmod(0o755)
        image = Path(temp) / "candidate.apfs"
        subprocess.run([sys.executable, str(KERNEL / "tools/apfs/mkapfs.py"), "-z", "--plain", "fw",
                        "-o", str(image), "hackios=" + str(stage)], check=True, stdout=subprocess.DEVNULL)
        if contents(image) != expected:
            raise ValueError("Rebuilt APFS entry inventory differs")
        digest = sha(image)
        target = output / ("cell-settings-" + digest + ".apfs")
        if target.exists():
            if sha(target) != digest: raise ValueError("Existing output digest mismatch")
        else:
            image.chmod(0o400)
            image.replace(target)
        record = dict(base=str(args.base.resolve()), base_sha256=sha(args.base),
                      root=str(target), root_sha256=digest, daemon_sha256=expected[DAEMON][3],
                      preserved_entries=len(original) - len(replaced), replaced_entry=DAEMON)
        if args.preferences_library is not None:
            record.update(replaced_entries=replaced, added_entries=[PREFERENCES_LIBRARY],
                          preferences_library_sha256=expected[PREFERENCES_LIBRARY][3],
                          original_tweaks_sha256=original[TWEAKS_LIBRARY][3],
                          original_inject_conf_sha256=original[INJECT_CONF][3],
                          original_inject_conf_bytes_preserved=len(prefix),
                          inject_conf_sha256=expected[INJECT_CONF][3],
                          exec_inject_bytes_including_nul=sysctl_bytes)
        (output / "root.json").write_text(json.dumps(record, indent=2) + "\n")
        print(json.dumps(record, indent=2))

if __name__ == "__main__":
    main()
