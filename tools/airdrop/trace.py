"""Summarize only AirDrop sender and AWDL ioctl evidence in one live or terminal kernel log."""
import argparse
import json
from pathlib import Path
import re

REQUEST = re.compile(r'apple80211: (\S+) (sets|gets) (94|95) \((VIRTUAL_IF_CREATE|VIRTUAL_IF_DELETE)\), value (-?\d+), (\d+) bytes.*: (answered|REFUSED|error (\d+))')
TOTAL = re.compile(r'apple80211: in this run: (set|get) (94|95) \((VIRTUAL_IF_CREATE|VIRTUAL_IF_DELETE)\) (\d+) times, answered (\d+) times(?:; the last refusal: (\d+))?')
SPAWN = re.compile(r'(\d\d:\d\d:\d\d\.\d+).*?Successfully spawned (AirDropTest|AirDrop)\[(\d+)\]')
FILE = re.compile(r'airdrop-test: (selected_readonly_file|native_share_sheet_presented) bytes=(\d+) sha256=([0-9a-f]{64})(?:\s|$)')
RF = re.compile(r'awdl: radio detached, RX actions=(\d+) psf=(\d+) mif=(\d+)')
DAEMON = re.compile(r'(?:20\d\d-\d\d-\d\d )?(\d\d:\d\d:\d\d(?:\.\d+)?)\S*\s+(sharingd|wifid)\[\d+(?::\d+)?\]')
RELEVANT = re.compile(r'\b(?:awdl\d*|airdrop|AirLink)\b|_airdrop\._tcp',re.I)
FAILURE = re.compile(r'fail(?:ed|ure)?|error|refus|not (?:found|supported|available)|unavailable|denied|timed out',re.I)
DOMAIN = re.compile(r'\b(NSPOSIXErrorDomain|NSOSStatusErrorDomain|NSCocoaErrorDomain|NSURLErrorDomain)\s+Code=(-?\d+)\b')
SIGNALS = {
    'Bonjour browser failed to find awdl0 interface': 'bonjour_missing_awdl',
    'AWDL is not supported': 'awdl_capability_unavailable',
    'No AWDL interface found': 'awdl_interface_absent',
    'No virtual interface name returned': 'virtual_interface_name_missing',
    'Cannot bind 80211 device': 'wireless_bind_failed',
    'Unable to start monitoring events': 'wireless_event_monitor_failed',
    'AWDL network failed to start': 'awdl_start_failed',
}

def report(path):
    lines=path.read_text(errors='replace').splitlines()
    starts=[i for i,line in enumerate(lines) if line.startswith('== KERNEL LOG')]
    if not starts: raise ValueError('kernel-log section missing: '+str(path))
    info=dict(source=path.name, launches=[],sender=[],request_lines=[],kernel_totals=[],passive_rx=[],service_errors=[],bonjour_missing_interface=False)
    for number,line in enumerate(lines[starts[-1]+1:],start=starts[-1]+2):
        m=REQUEST.search(line)
        if m:
            caller,op,kind,_,value,length,status,error=m.groups()
            info['request_lines'].append(dict(line=number,caller=caller,op=op,kind=int(kind),value=int(value),length=int(length),status=status))
        m=TOTAL.search(line)
        if m:
            op,kind,_,count,answered,error=m.groups()
            info['kernel_totals'].append(dict(line=number,op=op,kind=int(kind),count=int(count),answered=int(answered),last_error=int(error) if error else None))
        m=SPAWN.search(line)
        if m:info['launches'].append(dict(line=number,time=m[1],process=m[2],pid=int(m[3])))
        m=FILE.search(line)
        if m:info['sender'].append(dict(line=number,event=m[1],bytes=int(m[2]),sha256=m[3]))
        m=RF.search(line)
        if m:info['passive_rx'].append(dict(line=number,actions=int(m[1]),psf=int(m[2]),mif=int(m[3])))
        daemon=DAEMON.search(line)
        if daemon:
            signal=next((label for phrase,label in SIGNALS.items() if phrase in line), None)
            if signal or (RELEVANT.search(line) and FAILURE.search(line)):
                # Keep time, reporting daemon, a fixed label and known numeric
                # error codes. Never copy arbitrary messages, peer names or URLs.
                event=dict(line=number,time=daemon[1],process=daemon[2],signal=signal or 'airdrop_or_awdl_error')
                domain=DOMAIN.search(line)
                if domain: event.update(domain=domain[1],code=int(domain[2]))
                info['service_errors'].append(event)
                if signal=='bonjour_missing_awdl':info['bonjour_missing_interface']=True
    return info

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('log',type=Path)
    print(json.dumps(report(parser.parse_args().log),indent=2))
