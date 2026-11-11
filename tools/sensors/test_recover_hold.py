import argparse,contextlib,json,os,subprocess,tempfile,time,unittest
from pathlib import Path
from unittest.mock import patch
import cold_recover as c
import quarantine_state as qs
import recover_hold as h

def ses(i,port=21):return dict(locationID=port,IORegistryEntryID=200+i,sessionID=900+i)
class Phone:
    def __init__(self,start=None,state='idle'):
        self.serial=start or c.LOADER;self.i=1;self.state=state;self.now=0;self.writes=[];self.notify=[]
        self.spoof={};self.seen=None;self.bad_product=False;self.after_reset=None
    def devices(self):return self.seen if self.seen is not None else {self.serial}
    def var(self,serial,key):
        assert serial==self.serial
        if key=='product':return ('wrong' if self.bad_product else 'kona') if serial==c.STOCK else 'xnuloader'
        return self.state
    def io(self,*args,**kw):
        self.writes.append(args);assert args[0]=='-s' and args[1]==self.serial
        if args[2:] == ('reboot','bootloader'):
            assert self.serial==c.LOADER;self.serial=c.STOCK
            if self.after_reset:self.after_reset()
        else:
            assert self.serial==c.STOCK and args[2]=='boot';self.serial=c.LOADER;self.state='idle'
        self.i+=1
        return 'OKAY'
    def usb(self,serial):
        assert serial==self.serial
        return self.spoof.get(self.i,ses(self.i))
    def sleep(self,s):self.now+=s
    def run(self,prior=None,auth=lambda:None):
        with patch.object(c,'fastboot',side_effect=self.io),patch.object(c,'devices',side_effect=self.devices),patch.object(c,'variable',side_effect=self.var),patch.object(c.time,'sleep',side_effect=self.sleep),patch.object(c.time,'monotonic',side_effect=lambda:self.now):
            return h.prove(prior or ses(0),3,lambda e,**k:self.notify.append(e),auth,self.usb)
class Recovery(unittest.TestCase):
    def test_loader(self):
        p=Phone();s=p.run();self.assertEqual([v['stage'] for v in s],['source','reset-request','stock','developer-boot','idle']);self.assertEqual(s[-1]['session'],ses(3))
    def test_initial_stock_is_not_reset_proof(self):
        p=Phone(c.STOCK);s=p.run();self.assertEqual([x[2] for x in p.writes],['boot','reboot','boot']);self.assertEqual(s[-1]['session'],ses(4))
    def test_hung_reset_only(self):
        p=Phone(state='hung 43 9');s=p.run();self.assertEqual(p.writes[0],('-s',c.LOADER,'reboot','bootloader'));self.assertEqual(s[-1]['stage'],'idle')
    def test_running_unknown_refused(self):
        for state in ['running','hung','hung 1 2 extra','']:
            p=Phone(state=state)
            with self.assertRaises(c.RecoveryError):p.run()
            self.assertFalse(p.writes)
    def test_old_idle_is_not_proof(self):
        p=Phone();p.spoof[3]=ses(0)
        with self.assertRaises(c.RecoveryError):p.run()
    def test_wrong_port_stale_session_or_registry_id(self):
        for index,wrong in [(1,ses(1,22)),(2,ses(2,22)),(3,ses(3,22)),(2,dict(ses(2),sessionID=901)),(3,dict(ses(3),IORegistryEntryID=202))]:
            p=Phone();p.spoof[index]=wrong
            with self.assertRaises(c.RecoveryError):p.run()
    def test_absent_and_wrong_target(self):
        for seen in (set(),{'other'},{c.LOADER,c.STOCK},{'other',c.LOADER}):
            p=Phone();p.seen=seen
            with self.assertRaises(c.RecoveryError):p.run()
            self.assertFalse(p.writes)
            if not seen:self.assertIn('pending-no-endpoint',p.notify)
    def test_wrong_stock_product(self):
        p=Phone();p.bad_product=True
        with self.assertRaises(c.RecoveryError):p.run()
        self.assertFalse(any(v[2]=='boot' for v in p.writes))
    def test_superseding_hold_never_boots(self):
        p=Phone();flag=[True];p.after_reset=lambda:flag.clear()
        with self.assertRaises(c.RecoveryError):p.run(auth=lambda:h.guard(flag,'new hold'))
        self.assertFalse(any(v[2]=='boot' for v in p.writes))
    def test_status_migration_and_atomic_commit(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);queue=root/'queue';logs=root/'logs';queue.mkdir();logs.mkdir()
            rec=root/'out/eye/sensor-ticket-18';rec.mkdir(parents=True)
            now=time.time();(rec/'end-cold.json').write_text(json.dumps(dict(ticket=18,epoch=now-5,session=ses(0))))
            t=dict(id=19,state='ended',who='wifi-pm',rc=1,runs=['111111'],began=now-4,ended=now)
            (queue/'history.jsonl').write_text(json.dumps(dict(id=18,who='sensor',state='ended',rc=0,began=now-6,ended=now-4.9))+'\n'+json.dumps(t)+'\n')
            (queue/'cool_until').write_text(str(now+qs.WEEK));(queue/'cool').touch()
            f=logs/'111111-run.log';f.write_text('cmdline: debug=1 board=instantnoodle\ncold recovery required\n')
            with patch.object(qs.sys,'platform','darwin'):hold=qs.state(root,queue,logs)
            self.assertEqual(hold['ticket'],19);qs.pin(queue,hold)
            b=h.bootq();b.DIR=str(queue);b.ROOT=str(root);b.LOGS=str(logs)
            (queue/'ticket-20.json').write_text(json.dumps(dict(id=20,state='running',pid=os.getppid(),recovery_for=19,recovery_generation=hold['generation'])))
            folder=queue/'recovery-20';folder.mkdir();stages=Phone().run()
            qs.pin(queue,dict(hold,generation='new-generation'))
            with self.assertRaises(c.RecoveryError):h.commit(b,20,hold,stages,folder)
            self.assertFalse((folder/'receipt.json').exists());self.assertTrue((queue/'in2010-quarantine.json').exists())
            qs.pin(queue,hold);h.commit(b,20,hold,stages,folder)
            self.assertTrue((folder/'receipt.json').is_file());self.assertTrue((queue/'cool').exists());self.assertFalse((queue/'in2010-quarantine.json').exists())
            self.assertEqual(float((queue/'cool_until').read_text()),hold['normal_until'])
            (queue/'cool_until').write_text(str(now+qs.WEEK));f.write_text('cmdline: board=redwood\ncold recovery required\n')
            self.assertIsNone(qs.state(root,queue,logs))
            with patch.object(qs.sys,'platform','linux'):self.assertIsNone(qs.state(root,queue,logs))
    def test_fixed_queue_entry(self):
        b=h.bootq()
        with patch.dict(os.environ,{},clear=True),patch.object(b,'cmd_run',return_value=0) as go:
            self.assertEqual(b.cmd_recover(argparse.Namespace(ticket=19,seconds=7)),0)
            a=go.call_args.args[0];self.assertFalse(a.preempt or a.owner);self.assertEqual(a.recover_id,19);self.assertEqual(len(a.command),1);self.assertIn('tools/sensors/recover_hold.py',a.command[0])
        with patch.dict(os.environ,{'BOOTQ_TICKET':'1'}),self.assertRaises(SystemExit):b.cmd_recover(argparse.Namespace(ticket=19,seconds=7))
if __name__=='__main__':unittest.main()
