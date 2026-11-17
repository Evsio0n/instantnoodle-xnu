"""Retain a boot controller's rolling radio log without racing its empty file."""
import argparse
import re
import time
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('control',type=Path)
p.add_argument('--seconds',type=int,default=1000)
args=p.parse_args()
end=time.monotonic()+args.seconds
while time.monotonic()<end:
    text=args.control.read_text() if args.control.exists() else ''
    destination=re.search(r'^RESULT_DIR (\S+)',text,re.M)
    log=re.search(r'^LOG (\S+)',text,re.M)
    if destination and log:
        break
    time.sleep(.3)
else:
    raise SystemExit('controller never supplied both result directory and log')
seen=set()
tags=('awdl-wmi-raw:','awdl-dwell:','awdl-grant:','awdl-mif-tx:',
      'awdl-mif-done:','awdl-slot:','awdl-visit:','awdl-offload:',
      'awdl-service:','awdl-roc-obsolete:','awdl-slot-lag:','awdl-rx-path:','awdl-query:','awdl-flight-stall:','awdl-queue-age:')
with (Path(destination[1])/'destination-events.txt').open('w') as output:
    while time.monotonic()<end:
        file=Path(log[1])
        text=file.read_text(errors='replace') if file.exists() else ''
        for line in text.splitlines():
            if any(tag in line for tag in tags) and line not in seen:
                output.write(line+'\n');seen.add(line)
        output.flush()
        time.sleep(.4)
