"""Record native DNS-SD PTR callback TTL and removals on the Mac AWDL interface."""
import argparse
import ctypes as C
import json
import select
import socket
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=120)
    args = parser.parse_args()
    dns = C.CDLL('/usr/lib/libSystem.B.dylib')
    ref = C.c_void_p()
    handler = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_uint32, C.c_int32,
                          C.c_char_p, C.c_uint16, C.c_uint16, C.c_uint16,
                          C.c_void_p, C.c_uint32, C.c_void_p)

    @handler
    def record(_ref, flags, interface, error, _name, kind, _cls, length, data, ttl, _ctx):
        if error or kind != 12 or not length or not data:
            return
        rdata = C.string_at(data, length)
        size = rdata[0]
        if not 0 < size <= min(63, length - 1):
            return
        print(json.dumps(dict(time=time.time(), interface=interface, add=bool(flags & 2),
                              instance=rdata[1:size+1].decode('ascii','replace'), ttl=ttl)), flush=True)

    dns.DNSServiceQueryRecord.argtypes = [C.POINTER(C.c_void_p), C.c_uint32, C.c_uint32,
                                          C.c_char_p, C.c_uint16, C.c_uint16, handler, C.c_void_p]
    result = dns.DNSServiceQueryRecord(C.byref(ref), 0x100000, socket.if_nametoindex('awdl0'),
                                       b'_airdrop._tcp.local.', 12, 1, record, None)
    if result:
        raise OSError(result, 'DNSServiceQueryRecord')
    try:
        fd = dns.DNSServiceRefSockFD(ref)
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            if select.select([fd], [], [], max(0,end-time.monotonic()))[0]:
                result = dns.DNSServiceProcessResult(ref)
                if result:
                    raise OSError(result, 'DNSServiceProcessResult')
    finally:
        dns.DNSServiceRefDeallocate(ref)

if __name__ == '__main__':
    main()
