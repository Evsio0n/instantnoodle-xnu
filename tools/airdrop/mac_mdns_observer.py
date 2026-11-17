"""Observe only the current test phone's native IPv6 mDNS on this Mac's AWDL.

No service or AirDrop response is advertised. A 12-byte empty DNS datagram
from a separate local socket checks that the multicast receive path works;
it is counted separately and cannot be mistaken for a phone packet.
"""
import argparse,hashlib,ipaddress,json,socket,struct,time

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('phone_network');p.add_argument('--seconds',type=int,default=90)
    a=p.parse_args();assert 1<=a.seconds<=180
    data=json.load(open(a.phone_network))['result']['data']['interfaces']
    expected={str(ipaddress.IPv6Address(x['address'].split('%')[0])) for x in data if x['name']=='awdl0' and x['family']==30}
    if len(expected)!=1:raise SystemExit('one current phone awdl0 address required')
    index=socket.if_nametoindex('awdl0');group='ff02::fb'
    receiver=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM)
    sender=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM)
    for s in (receiver,sender):
        # Stable XNU socket ABI: unrestricted receive and AWDL opt-in.
        s.setsockopt(socket.SOL_SOCKET,0x1104,1);s.setsockopt(socket.SOL_SOCKET,0x1113,1)
    receiver.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    receiver.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEPORT,1)
    receiver.bind((group,5353,0,index))
    receiver.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_JOIN_GROUP,
        socket.inet_pton(socket.AF_INET6,group)+struct.pack('@I',index))
    receiver.settimeout(.5)
    sender.bind(('::',0));sender.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_MULTICAST_IF,index)
    sender.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_MULTICAST_LOOP,1)
    sender.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_MULTICAST_HOPS,255)
    control=b'\0'*12;control_port=sender.getsockname()[1]
    try:sender.sendto(control,(group,5353,0,index));print('observer_control_submitted=1',flush=True)
    except OSError as e:print('observer_control_errno='+str(e.errno),flush=True)
    deadline=time.monotonic()+a.seconds;own=hits=0
    print('observer_ready awdl_opt_in=1 service_registration=0 response_sender=0',flush=True)
    try:
        while time.monotonic()<deadline:
            try:packet,source=receiver.recvfrom(65535)
            except socket.timeout:continue
            if source[1]==control_port and packet==control:
                own+=1;print('observer_local_receive_validated=1',flush=True);continue
            if str(ipaddress.IPv6Address(source[0].split('%')[0])) not in expected:continue
            hits+=1;print('phone_mdns_received '+json.dumps(dict(bytes=len(packet),airdrop_query=b'_airdrop' in packet,
                sha256=hashlib.sha256(packet).hexdigest())),flush=True)
    finally:receiver.close();sender.close()
    print(json.dumps(dict(observer_local_controls=own,phone_packets=hits)),flush=True)
if __name__=='__main__':main()
