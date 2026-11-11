"""Run the real shared FIFO algorithm against isolated files and a dummy non-USB command."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch
import transport as t


class Reservation(unittest.TestCase):
    def test_dead_worker_and_owner_preemption_cannot_release_hold(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);queue=d/'q';queue.mkdir();logs=d/'logs';logs.mkdir()
            env=dict(os.environ,BOOTQ_DIR=str(queue),BOOTQ_LOGS=str(logs),BOOTQ_PHONE=str(d/'phone'),
                     BOOTQ_GETVAR='true',BOOTQ_STOP=f'touch {d}/stopped')
            with patch.dict(os.environ,env):
                b=t.recovery.bootq()
                # This dead portable producer is deliberately removed by Queue.tickets().
                with b.Queue() as q:
                    q.save(dict(id=41,pid=99999999,priority=1,state='running',who='portable',what='test'))
                    t.qs.pin(queue,dict(ticket=41,generation='g',runs=[],prior={'session':{}},portable_id='test'))
            bootq=t.ROOT/'tools/bootq'
            cmd=[str(bootq),'run','--who','owner','--what','mock','--owner','--preempt','--no-boot-ok',
                 '--wait','2','--',f'touch {d}/ran']
            r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=9)
            self.assertEqual(r.returncode,124,r.stdout+r.stderr)
            subprocess.run([bootq,'cool','off'],env=env,capture_output=True,check=True)
            state=json.loads(subprocess.check_output([bootq,'status','--json'],env=env))
            self.assertEqual(state['quarantine']['ticket'],41)
            self.assertFalse((d/'ran').exists());self.assertFalse((d/'stopped').exists())


if __name__=='__main__':unittest.main()
