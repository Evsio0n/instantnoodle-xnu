"""Package the audited immutable OnePlus sensor configs for a private RAM root."""
import hashlib
import json
from pathlib import Path
import re
import sys

SELECTORS = {"hw_platform","platform_subtype","platform_subtype_id","platform_version","soc_id","revision"}

def pack(manifest, output, header, selectors=None, vendor=None):
    data = bytearray()
    entries = []
    seen = set()
    rows = json.loads(manifest.read_text())["static_readonly_files"]
    if selectors:
        extra = json.loads(selectors.read_text())["static_readonly_files"]
        if {r["target_path"] for r in extra} != {"/sys/devices/soc0/"+key for key in SELECTORS}:
            raise ValueError("incomplete selector manifest")
        rows += extra
    if vendor:
        extra=json.loads(vendor.read_text())["static_readonly_files"]
        if not extra or any(not re.fullmatch(r"/vendor/etc/sensors/config/[A-Za-z0-9_.-]+\.json",r["target_path"]) for r in extra):
            raise ValueError("invalid vendor config manifest")
        rows += extra
    for row in rows:
        path = row["target_path"]
        if not (path in {"/sys/devices/soc0/"+key for key in SELECTORS} or path == "/vendor/etc/sensors/sns_reg_config" or
                re.fullmatch(r"(?:/odm/etc/sensor/config|/vendor/etc/sensors/config)/[A-Za-z0-9_.-]+\.json", path)):
            raise ValueError(f"unexpected target {path}")
        content = Path(row["source_path"]).read_bytes()
        if path in seen or not content or len(content) != row["bytes"] or hashlib.sha256(content).hexdigest() != row["sha256"]:
            raise ValueError(f"manifest mismatch for {path}")
        seen.add(path)
        entries.append((path,len(data),len(content)))
        data.extend(content)
    if not data or len(data)>1024*1024:
        raise ValueError("invalid bundle size")
    output.write_bytes(data)
    digest = hashlib.sha256(data).digest()
    header.write_text("/* Generated from registry-bundle-manifest.json. */\n"
        +f"#define SSC_PACK_SIZE {len(data)}u\n"
        +"static const uint8_t ssc_pack_sha[32]={"+",".join(hex(v) for v in digest)+"};\n"
        +"static const struct ssc_entry ssc_entries[]={\n"
        +"".join(f'    {{"{p}", {o}u, {n}u}},\n' for p,o,n in entries)+"};\n")
    print(f"{len(entries)} files, {len(data)} bytes, {digest.hex()}")

if __name__ == "__main__":
    pack(*(Path(s) for s in sys.argv[1:]))
