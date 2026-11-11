#!/usr/bin/env python3
"""lg.py LOG [CODEHEX...]: the modem's log packets in a run's log ("modem: [ms] lg: N at OFF: HEX", printed whole by the kernel after a DIAG request
by hand), put together again: time, code, length, then the body in hex and in letters. LTE NAS messages (B0E2/B0E3 ESM, B0EC/B0ED EMM) get their
message type named and, for ESM, the PDN type, the cause and the kinds of containers in the protocol configuration options. Addresses are not printed
(carrier-assigned ones are not written down): only their kind and count."""
import re, sys
ESM = {0xc1: 'ACTIVATE DEFAULT BEARER REQUEST', 0xc2: 'ACTIVATE DEFAULT BEARER ACCEPT', 0xc3: 'ACTIVATE DEFAULT BEARER REJECT', 0xc5: 'ACTIVATE DEDICATED BEARER REQUEST',
       0xc6: 'ACTIVATE DEDICATED BEARER ACCEPT', 0xc9: 'MODIFY BEARER REQUEST', 0xca: 'MODIFY BEARER ACCEPT', 0xcd: 'DEACTIVATE BEARER REQUEST', 0xce: 'DEACTIVATE BEARER ACCEPT',
       0xd0: 'PDN CONNECTIVITY REQUEST', 0xd1: 'PDN CONNECTIVITY REJECT', 0xd2: 'PDN DISCONNECT REQUEST', 0xd3: 'PDN DISCONNECT REJECT', 0xd9: 'ESM INFORMATION REQUEST',
       0xda: 'ESM INFORMATION RESPONSE', 0xe8: 'ESM STATUS'}
PCO = {0x0001: 'P-CSCF IPv6', 0x0002: 'IM CN flag', 0x0003: 'DNS IPv6', 0x0005: 'bearer control', 0x000a: 'IP via NAS', 0x000c: 'P-CSCF IPv4', 0x000d: 'DNS IPv4',
       0x0010: 'IPv4 MTU', 0x0012: 'P-CSCF reselection', 0x8021: 'IPCP', 0xc023: 'PAP', 0xc223: 'CHAP'}

def pco(v):
    out = []; i = 1
    while i + 3 <= len(v):
        cid = v[i] << 8 | v[i + 1]; l = v[i + 2]; out.append('%s(%d)' % (PCO.get(cid, '%04x' % cid), l)); i += 3 + l
    return ' '.join(out)

def esm(m):
    if len(m) < 3: return ''
    t = m[2]; s = 'bearer %d pti %d %s' % (m[0] >> 4, m[1], ESM.get(t, '%02x' % t)); i = 3
    if t == 0xd0 and len(m) > 3:
        s += ' | PDN type asked %d, request type %d' % (m[3] >> 4, m[3] & 15); i = 4
    elif t == 0xc1:
        l = m[i]; i += 1 + l                                        # EPS QoS
        l = m[i]; apn = m[i + 1:i + 1 + l]; i += 1 + l              # the APN, labels
        l = m[i]; s += ' | APN %s, PDN type given %d' % (re.sub(rb'[^ -~]', b'.', apn[1:]).decode(), m[i + 1] & 7); i += 1 + l
    elif t in (0xd1, 0xcd, 0xc3) and len(m) > i:
        s += ' | cause %d' % m[i]; i += 1
    elif t == 0xd2 and len(m) > i:
        s += ' | linked bearer %d' % (m[i] & 15); i += 1
    while i < len(m):                                              # the optional parts
        iei = m[i]
        if iei >> 4 in (0xd, 0x8, 0x9, 0xa, 0xb, 0xc) and iei not in (0x80,):       # (half-byte ones)
            i += 1; continue
        if i + 1 >= len(m): break
        l = m[i + 1]; v = m[i + 2:i + 2 + l]
        if iei == 0x58: s += ' | cause %d' % m[i + 1]; i += 2; continue
        if iei == 0x32: i += 2; continue                           # (LLC SAPI: a type and one byte)
        if iei == 0x7b and i + 2 < len(m):                         # (extended options: two bytes of length)
            l = m[i + 1] << 8 | m[i + 2]; s += ' | extended options: ' + pco(m[i + 3:i + 3 + l]); i += 3 + l; continue
        if iei == 0x27: s += ' | options: ' + pco(v)
        if iei == 0x28: s += ' | APN ' + re.sub(rb'[^ -~]', b'.', v[1:]).decode()
        i += 2 + l
    return s

rows = {}
want = {int(x, 16) for x in sys.argv[2:]}
cur = None; packets = []
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[ *(\d+) ms\] (lg|ev): (\d+) at (\d+): ([0-9a-f]+)', l)
    if not m: continue
    ms, kind, n, at, hx = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
    if at == 0:
        cur = [ms, kind, n, bytearray()]; packets.append(cur)
    if cur is not None: cur[3] += bytes.fromhex(hx)
for ms, kind, n, b in packets:
    if kind == 'ev':
        print('[%7d ms] events %d bytes: %s' % (ms, n, b.hex())); continue
    if len(b) < 16: continue
    code = b[6] | b[7] << 8; body = bytes(b[16:])
    if want and code not in want: continue
    text = ''
    if code in (0xb0e2, 0xb0e3): text = esm(body[4:])
    elif code in (0xb0ec, 0xb0ed): text = 'EMM type %02x' % (body[4 + 1] if len(body) > 5 and body[4] & 0xf0 == 0 else (body[4 + 7] if len(body) > 11 else 0))
    if code >> 12 == 0xb and code in (0xb0e2, 0xb0e3, 0xb0ec, 0xb0ed):
        print('[%7d ms] log %04x %s %s' % (ms, code, 'down' if code in (0xb0e2, 0xb0ec) else 'up', text))
    else:
        letters = re.sub(rb'[^ -~]', b'.', body).decode()
        print('[%7d ms] log %04x %d bytes: %s\n             %s' % (ms, code, len(body), body.hex()[:600], letters[:900]))
