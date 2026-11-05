#!/usr/bin/env python3
"""ios_metallib.py IN OUT: a metallib this Mac's Metal compiler made, in the words iOS 16.5's Metal will load.

Run 122910: `newLibraryWithData:` on the phone answers "This library format is not supported on this
platform (or was built with an old version of the tools)" for what airdump.m serializes. The header
says what it was built for, and comparing it with iOS 16.5's own libraries (AV1SW, AppleMetalGLRenderer):

    Mac   01 80  02 00 09 00  00 81  1b 00 00 00    arm64, macOS, library 2.9, platform 27.0
    iOS   01 00  02 00 07 00  00 82  10 00 05 00    library 2.7, iOS, platform 16.5

and each function's VERS tag (AIR 2.9, Metal 2.4) against iOS's (AIR 2.5, Metal 3.0). Those words are
all this changes. The bitcode is left as it is, and this port's own frontend reads that; the frontend
has been run over it (air2nir). Nothing here makes the library right for a real iOS device.
"""
import struct
import sys


def main():
    data = bytearray(open(sys.argv[1], 'rb').read())
    if data[:4] != b'MTLB':
        sys.exit('not a metallib')
    data[4:16] = bytes.fromhex('0100 0200 0700 0082 1000 0500')
    list_off, list_size = struct.unpack('<QQ', data[0x18:0x28])
    count = struct.unpack('<I', data[list_off:list_off + 4])[0]
    at = list_off + 4
    patched = 0
    for _ in range(count):
        size = struct.unpack('<I', data[at:at + 4])[0]
        i = at + 4
        while i < at + size:
            tag = bytes(data[i:i + 4])
            if tag == b'ENDT':
                break
            length = struct.unpack('<H', data[i + 4:i + 6])[0]
            if tag == b'VERS':
                data[i + 6:i + 6 + 8] = struct.pack('<HHHH', 2, 5, 3, 0)
                patched += 1
            i += 6 + length
        at += size
    open(sys.argv[2], 'wb').write(data)
    print(f'{count} functions, {patched} VERS tags, header for iOS 16.5')


main()
