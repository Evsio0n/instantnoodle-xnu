#!/usr/bin/env python3
"""Generate synthetic RFC 4861 packets and invoke the real C classifier."""
import ctypes
import ipaddress
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
local = ipaddress.IPv6Address("fe80::1001").packed
peer = ipaddress.IPv6Address("fe80::2002").packed
solicited = ipaddress.IPv6Address("ff02::1:ff00:1001").packed
all_nodes = ipaddress.IPv6Address("ff02::1").packed
mac = bytes.fromhex("020000002002")
slla = bytes([1, 1]) + mac
tlla = bytes([2, 1]) + mac


def packet(kind=135, dst=solicited, target=local, options=slla, flags=0, src=peer, hlim=255, code=0):
    message = struct.pack("!BBHI", kind, code, 0, flags) + target + options
    pseudo = src + dst + struct.pack("!I3xB", len(message), 58)
    checksum_bytes = pseudo + message
    if len(checksum_bytes) % 2:
        checksum_bytes += b"\0"
    words = struct.unpack(f"!{len(checksum_bytes)//2}H", checksum_bytes)
    checksum = sum(words)
    while checksum >> 16:
        checksum = (checksum & 65535) + (checksum >> 16)
    message = message[:2] + struct.pack("!H", checksum ^ 65535) + message[4:]
    return struct.pack("!IHBB", 6 << 28, len(message), 58, hlim) + src + dst + message


with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "nd6.so"
    src = b'#include "nd6_observe.h"\nunsigned check(const uint8_t*p,size_t n,const uint8_t*l,const uint8_t*r,const uint8_t*m){return lx_awdl_nd6_observe(p,n,l,r,m);}\n'
    subprocess.run(["cc", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
                    "-I", str(HERE), "-std=c11", "-x", "c", "-", "-o", str(binary)], input=src, check=True)
    check = ctypes.CDLL(str(binary)).check
    check.argtypes = [ctypes.c_char_p, ctypes.c_size_t] + [ctypes.c_char_p] * 3
    check.restype = ctypes.c_uint
    cases = [(packet(), 1), (packet(dst=local, options=b""), 1),
             (packet(136, local, local, tlla, 0x40000000), 2),
             (packet(136, local, peer, tlla, 0x40000000), 3),
             (packet(136, all_nodes, peer, tlla), 3),
             (packet(136, all_nodes, peer, tlla, 0x40000000), 0),
             (packet(options=b""), 0), (packet(dst=all_nodes), 0),
             (packet(target=peer), 0), (packet(src=local), 0), (packet(hlim=254), 0),
             (packet(code=1), 0), (packet(options=bytes([1, 1]) + mac[::-1]), 0),
             (packet(options=tlla), 0), (packet(options=b"\x01\x00" + mac), 0),
             (packet(options=b"\x01\x02" + mac), 0),
             (packet(options=b"\xfa\x01" + bytes(6) + slla), 1),
             (packet(options=slla + b"\x01"), 0)]
    altered = bytearray(packet())
    altered[43] ^= 1
    cases += [(bytes(altered), 0), (packet()[:-1], 0), (packet() + b"\0", 0)]
    for data, expected in cases:
        actual = check(data, len(data), local, peer, mac)
        assert actual == expected, (actual, expected)
    print(f"PASS {len(cases)} synthetic ND6 cases")
