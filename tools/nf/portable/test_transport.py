"""No USB, modem, host queue or network. Exercise production receipt and observation paths."""
import contextlib
import io
import time
import unittest
from unittest.mock import patch
import transport as t

ID='ab'*16
STOCK=dict(locationID=7,sessionID=1,IORegistryEntryID=9)
DEV=dict(locationID=7,sessionID=2,IORegistryEntryID=10)
NEW=dict(locationID=7,sessionID=3,IORegistryEntryID=11)
READY=f'portable-ready-v1 id={ID} end\n'.encode()
DONE=f'portable-rehearsal-v1 id={ID} completed=1 end\n'.encode()
GNSS=f'portable-gnss-v1 id={ID} started=1 complete=1 reports=9 fix=0 end\n'.encode()
OFF=t.PINS+b' CLKREQ\nTHE WARM RESET IS DONE\nmodem: level 3 is done\n'
TIMED=b'--- loader_return_after expired ---\n'
RETURN=b'--- the host asked for the end of the run: back to the loader ---\n'


def session(mode='rehearsal'):
    return dict(id=ID,mode=mode,stock=STOCK,developer=DEV,cmdline=t.cmdline(mode,ID,1000000000))


def log(s,body,final,nonce=ID):
    header=b'== XNULOADER run (usb) ==\ncmdline: debug=0x8 board=instantnoodle '+s['cmdline'].replace(ID,nonce).encode()+b'\n'
    return header+(t.SEP+body+t.END if final else t.LIVE+body),f'returned {len(body)} 0' if final else 'running 0 0'


def simulate(s,sequence):
    now=[0];last=[None];stop=[]
    def seen():
        if not sequence:return None
        last[0]=sequence.pop(0)
        return t.cold.LOADER if last[0] else None
    def read(state):
        usb,body,final=last[0]
        return usb,t.info(*log(state,body,final),state)
    with contextlib.redirect_stdout(io.StringIO()):
        t.observe(s,27,seen=seen,read=read,clock=lambda:now[0],pause=lambda n:now.__setitem__(0,now[0]+n),
                  save=lambda state,**k:state.update(k),end_run=lambda:stop.append(1))
    return stop


class Portable(unittest.TestCase):
    def test_returned_after_disconnected(self):
        s=session();self.assertEqual(simulate(s,[(DEV,READY,False),None,None,(NEW,READY+DONE+TIMED,True)]),[])
        self.assertTrue(t.complete(s))
    def test_reconnect_while_running(self):
        s=session('gnss');self.assertEqual(simulate(s,[(DEV,READY,False),None,(NEW,GNSS+OFF,False),
                                                       (NEW,GNSS+OFF+RETURN,True)]),[1])
        self.assertTrue(t.complete(s));self.assertFalse(s['receipt']['fix'])
    def test_stay_absent_times_out_without_reset(self):
        s=session()
        with self.assertRaises(TimeoutError):simulate(s,[(DEV,READY,False),None,None])
        self.assertTrue(s.get('absent'));self.assertFalse(s.get('stop_requested'))
    def test_other_port_and_stale_session(self):
        for usb in (dict(NEW,locationID=19),dict(NEW,sessionID=2)):
            with self.subTest(usb=usb),self.assertRaises(ValueError):
                simulate(session(),[(DEV,READY,False),None,(usb,READY+DONE+TIMED,True)])
        s=session();simulate(s,[(DEV,READY,False),None,(DEV,READY+DONE+TIMED,True)]);self.assertFalse(t.complete(s))
    def test_unwitnessed_reenumeration(self):
        with self.assertRaises(ValueError):simulate(session(),[(DEV,READY,False),(NEW,READY+DONE+TIMED,True)])
    def test_wrong_nonce_and_footer(self):
        s=session()
        with self.assertRaises(ValueError):t.info(*log(s,DONE,True,'b'*32),s)
        raw,result=log(s,DONE,True)
        with self.assertRaises(ValueError):t.info(raw[:-4],result,s)
    def test_malformed_receipt_and_modalities(self):
        s=session('gnss')
        with self.assertRaises(ValueError):t.info(*log(s,GNSS+GNSS.replace(b'fix=0',b'fix=1'),True),s)
        with self.assertRaises(ValueError):t.info(*log(s,GNSS.replace(b'started=1',b'started=0'),True),s)
        with self.assertRaises(ValueError):t.info(*log(session(),GNSS,True),session())
    def test_partial_result_keeps_unverified(self):
        s=session('gnss');simulate(s,[(DEV,READY,False),None,(NEW,GNSS+TIMED,True)])
        self.assertFalse(t.complete(s))
    def test_hung_or_unsafe_never_accepted(self):
        for marker in t.UNSAFE:
            with self.subTest(marker=marker),self.assertRaises(ValueError):
                t.info(*log(session(),marker+DONE,True),session())
        with self.assertRaises(ValueError):t.info(log(session(),DONE,True)[0],'hung 121 0',session())
    def test_actual_cmdline_bound_and_disabled_writes(self):
        prefix=(t.ROOT.parent/'oneplus8-audit/gpu-plan/observed-loader-prefix.txt').read_bytes()
        for mode in t.TIMERS:
            s=session(mode)
            self.assertLessEqual(len(prefix)+len(s['cmdline']),591)
            self.assertIn('ufs_write=0',s['cmdline']);self.assertIn('var_store=0',s['cmdline'])
    def test_generated_payload_roundtrip(self):
        from tempfile import TemporaryDirectory
        from pathlib import Path
        files=t.artifacts()
        with TemporaryDirectory() as d:
            p=Path(d)/'ram.bin'
            t.payload(p,[('kernel',files['kernel']),('ramdisk',files['root'])],session()['cmdline'])
            self.assertLessEqual(p.stat().st_size,0x03f00000)
            b=p.read_bytes();self.assertEqual(b[:4],b'XNUP');self.assertIn(session()['cmdline'].encode()+b'\0',b)
    def test_ambiguous_serial(self):
        with patch.object(t.cold,'fastboot',return_value='xnuloader fastboot\nxnuloader fastboot\n'):
            with self.assertRaises(ValueError):t.endpoints()
    def test_private_rejection_and_real_plug_proof(self):
        s=session();s['profile']={'revision':2}
        ev=(f'portable-usb-v2 id={ID} physical=0 usb_event=1 end\n'
            f'portable-control-v2 id={ID} action=2 state=2 physical=1 event=1 errno=35 end\n'
            'portable-http-v2 action=2 outcome=kernel code=35 end\n').encode()
        simulate(s,[(DEV,READY,False),None,(NEW,READY+DONE+TIMED,True)])
        self.assertFalse(t.complete(s))
        simulate(s,[(NEW,READY+DONE+ev+TIMED,True)])
        self.assertEqual(s['cable_rows'],[(0,1)])
        self.assertEqual(s['http_rows'],[(2,'kernel',35)]);self.assertTrue(t.complete(s))

    def test_private_projection(self):
        s=session('gnss');result=t.info(*log(s,GNSS+b'ignored secret sentinel\n',True),s)
        self.assertNotIn('sentinel',str(result));self.assertEqual(set(result['receipt']),{'started','complete','reports','fix'})


if __name__=='__main__':unittest.main()
