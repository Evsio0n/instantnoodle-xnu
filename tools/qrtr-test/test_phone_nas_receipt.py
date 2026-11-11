import unittest
from verify_phone_nas import verify,P
class TestReceipt(unittest.TestCase):
    def setUp(self):
        self.lines=[P+'allocate epoch=10 cid1=1 local1=16390 cid2=2 local2=16391']
        for cid,port in ((1,16390),(2,16391)):
            line=f'epoch=10 cid={cid} local={port} node=3 peer=58 tid=30115 msg=36'
            self.lines.append(P+'submit '+line+' rc=0')
            self.lines.append(P+'reply '+line+' bytes=36 result=0 error=0 reg=0 cs=2 ps=2')
        self.lines.append(P+'end epoch=10 reason=both_replies attempts=2 replies=3 valid=3 released=1')
    def test_exact(self):
        self.assertTrue(verify(self.lines+self.lines)['checks_pass'])
    def test_mismatch(self):
        for a,b in [('local=16391','local=16390'),('tid=30115','tid=1'),('peer=58','peer=59'),('epoch=10','epoch=11'),
                    ('result=0','result=1'),('bytes=36','bytes=19')]:
            sample=list(self.lines);sample[-2]=sample[-2].replace(a,b)
            self.assertFalse(verify(sample)['checks_pass'],(a,b))
    def test_two_firmware_errors_preserve_transport_and_fail_data(self):
        rows=[s.replace('bytes=36 result=0 error=0 reg=0 cs=2 ps=2','bytes=22 result=1 error=74 reg=0 cs=0 ps=0')
                .replace('valid=3','valid=0') for s in self.lines]
        report=verify(rows)
        self.assertTrue(report['two_client_routing_verified'])
        self.assertFalse(report['checks_pass'])
        self.assertFalse(report['serving_data_verified'])
        self.assertEqual(report['serving_values'],[])
    def test_timeout(self):
        self.assertFalse(verify(self.lines[:-2]+[self.lines[-1].replace('both_replies','deadline')])['checks_pass'])
if __name__=='__main__':unittest.main()
