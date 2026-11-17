#!/usr/bin/env python3
"""Test the offline summary against Wireshark's pinned, synthetic AWDL capture."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FIXTURE = HERE.parents[2] / "oneplus8-audit/awdl-source-oracle/awdl_coverage.pcap.gz"
GZIP_SHA = "82e34577f516af60a963f84ef2aa257ffb646b0329ea63107601092d99afeeb5"
PCAP_SHA = "aa82c7717219edfe6fd9c034dedcaae500fbfcac4b2410e9acabbfbc44ebd8ee"
EXPECTED = dict(datalink=105, packets=24, psf=3, mif=12, other=9,
                truncated=0, bad_header=0, bad_fcs=0, plain_data_ipv6=4, complete_tlvs=15,
                malformed_tlvs=0, with_sync=3, with_election=2, with_sequence=2)


def capture(link, records):
    header = struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, link)
    parts = [header]
    for data, original_size in records:
        parts.extend((struct.pack("<IIII", 0, 0, len(data), original_size), data))
    return b"".join(parts)


def records(source):
    offset = 24
    frames = []
    while offset < len(source):
        size, original_size = struct.unpack_from("<II", source, offset + 8)
        offset += 16
        frame = source[offset:offset + size]
        assert len(frame) == size == original_size
        frames.append(frame)
        offset += size
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", nargs="?", type=Path, default=FIXTURE)
    args = parser.parse_args()
    compressed = args.fixture.read_bytes()
    assert hashlib.sha256(compressed).hexdigest() == GZIP_SHA
    source = gzip.decompress(compressed)
    assert hashlib.sha256(source).hexdigest() == PCAP_SHA
    frames = records(source)
    sample = frames[0]
    cases = []

    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "capture_summary"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(HERE / "capture_summary.c"), "-lpcap", "-o", str(binary)],
                       check=True)

        def summary(data, *options):
            proc = subprocess.run([str(binary), "-", *options], input=data,
                                  capture_output=True, check=True)
            return json.loads(proc.stdout)

        def expect(name, data, expected, *options):
            actual = summary(data, *options)
            for key, value in expected.items():
                assert actual[key] == value, (name, key, actual, expected)
            cases.append(name)

        expect("upstream_synthetic", source, EXPECTED)
        placeholder_fcs = bytes(4)
        raw = [(f + placeholder_fcs, len(f) + 4) for f in frames]
        expect("native_declared_fcs", capture(105, raw), EXPECTED, "--raw-fcs4")

        # An extended present bitmap requires alignment padding before TSFT.
        # Flags then says a trailing FCS was retained in the capture.
        radiotap = struct.pack("<BBHII", 0, 0, 25, 0x80000003, 0) + bytes(12) + b"\x10"
        wrapped = [(radiotap + f, len(radiotap) + n) for f, n in raw]
        expect("radiotap_tsft_flags_extension", capture(127, wrapped),
               {**EXPECTED, "datalink": 127})
        bad_fcs = radiotap[:-1] + b"\x50" + sample + placeholder_fcs
        expect("radiotap_bad_fcs", capture(127, [(bad_fcs, len(bad_fcs))]),
               dict(bad_fcs=1, psf=0))
        expect("incomplete_radiotap", capture(127, [(radiotap[:10], 10)]),
               dict(bad_header=1, psf=0))
        expect("capture_truncation", capture(105, [(sample[:-7], len(sample))]),
               dict(truncated=1, psf=0))
        expect("incomplete_tlv", capture(105, [(sample[:-1], len(sample) - 1)]),
               dict(psf=1, malformed_tlvs=1, complete_tlvs=0, with_sync=0))
        expect("ordered_action", capture(105, [(sample[:1] + b"\x80" + sample[2:24]
                                               + bytes(4) + sample[24:], len(sample) + 4)]),
               dict(psf=1, complete_tlvs=1))
        unsupported = subprocess.run([str(binary), "-"], input=capture(1, []),
                                     capture_output=True)
        assert unsupported.returncode == 2
        cases.append("reject_ethernet")

        typed = Path(tmp) / "test_rx_action"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(HERE / "test_rx_action.c"), "-lpcap", "-o", str(typed)],
                       check=True)
        typed_result = subprocess.run([str(typed)], input=source, capture_output=True,
                                      check=True).stdout.decode().strip()

    print(json.dumps({"synthetic_fixture_sha256": PCAP_SHA, "summary": EXPECTED,
                      "passed": cases, "typed_rx": typed_result}, indent=2))


if __name__ == "__main__":
    main()
