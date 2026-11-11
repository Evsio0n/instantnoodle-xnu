"""A separate queue-owned ordinary-loader hard reset, explicitly outside the RAM-log test."""
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
import transport as tr


def inside(predecessor):
    b=tr.recovery.bootq()
    tid=int(os.environ.get('BOOTQ_TICKET','0'))
    with tr.hardware_locks(b):
        with b.Queue():
            here=tr.qs.load(Path(b.DIR)/f'ticket-{tid}.json')
            tr.need(here['pid']==os.getppid() and here['state']=='running' and here['who']=='portable-handoff'
                    and not b.quarantine(),'must own the ordinary stock-transition ticket')
            history=[json.loads(row) for row in (Path(b.DIR)/'history.jsonl').read_text().splitlines()]
            source=next((t for t in reversed(history) if t.get('id')==predecessor),None)
            tr.need(source and source.get('state')=='ended' and not source.get('left_running') and source.get('runs')
                    and predecessor<tid,'predecessor has not reached a terminal hardware state')
        tr.need(tr.endpoints()==tr.cold.LOADER and tr.cold.variable(tr.cold.LOADER,'product')=='xnuloader'
                and tr.cold.variable(tr.cold.LOADER,'result')=='idle','ordinary idle Developer endpoint required')
        original=tr.recovery.mac_usb_session(tr.cold.LOADER)
        def witness(phase,serial):
            if phase in ('source','reset-request'):
                tr.need(serial==tr.cold.LOADER and tr.endpoints()==serial and tr.recovery.mac_usb_session(serial)==original
                        and tr.cold.variable(serial,'result')=='idle','source changed before stock transition')
            elif phase=='stock':
                tr.need(serial==tr.cold.STOCK and tr.recovery.fresh(original,tr.stock()),'stock proof is stale')
        tr.cold.recover(False,True,120,witness=witness)
        usb=tr.stock()
        tr.need(tr.recovery.fresh(original,usb),'stock USB generation changed before receipt')
        tr.qs.atomic(tr.OUTPUT/f'stock-{tid}.json',json.dumps(dict(ticket=tid,after=predecessor,session=usb,epoch=time.time())))
        print('Fresh exact stock FASTBOOT handoff confirmed.',flush=True)


if __name__=='__main__':
    os.umask(0o077)
    if len(sys.argv)!=3 or sys.argv[1] not in ('--after','--inside') or not sys.argv[2].isdigit():
        sys.exit('stock_handoff.py --after PREDECESSOR_TICKET')
    tr.need(sys.platform=='darwin' and not any(os.environ.get(k) for k in ('BOOTQ_DIR','BOOTQ_PHONE','BOOTQ_LOGS','BOOTQ_STOP','BOOTQ_GETVAR','ANDROID_SERIAL')),
            'normal macOS physical queue required')
    if sys.argv[1]=='--inside':inside(int(sys.argv[2]))
    else:
        tr.need(not os.environ.get('BOOTQ_TICKET'),'submit outside another ticket')
        cmd='exec '+shlex.join([sys.executable,str(Path(__file__).resolve()),'--inside',sys.argv[2]])
        sys.exit(subprocess.run([tr.ROOT/'tools/bootq','run','--who','portable-handoff','--what',
            'Verify cabled stock FASTBOOT before the no-modem detach rehearsal','--max','175','--no-boot-ok','--',cmd],
            cwd=tr.ROOT).returncode)
