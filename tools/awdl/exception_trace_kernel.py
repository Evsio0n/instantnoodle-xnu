"""Enable only the existing XNU exception-trace tunable in a pinned RAM kernel.

The source TUNABLE initializer is file-backed __DATA_CONST,__const. Startup
leaves it intact when exctrace is absent. No instructions, radio object,
Apple binary, or existing kernel file are changed; emit an immutable copy.
"""
import hashlib,json,struct,subprocess
from pathlib import Path
K=Path(__file__).resolve().parents[2]
OUT=K/'out/awdl-kernel/integration'
EXPECTED='f95cd6e76710b3e64a2fc08516e37b9ee3651615520890121794b7b98a160baa'
def sha(b):return hashlib.sha256(b).hexdigest()
def build():
 source=Path((OUT/'wmi-carrier-stable49.txt').read_text().strip());data=source.read_bytes()
 if sha(data)!=EXPECTED:raise ValueError('control kernel changed')
 symbols={}
 for line in subprocess.check_output(['nm','-an',str(source)],text=True).splitlines():
  f=line.split()
  if len(f)==3 and f[2] in ('_user_exception_trace_only','_user_exception_trace_enabled'):symbols[f[2]]=int(f[0],16)
 if len(symbols)!=2:raise ValueError('missing unambiguous existing tunables')
 def locate(address):
  at=32
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   cmd,n=struct.unpack_from('<II',data,at)
   if cmd==0x19:
    name,va,size,fo,fs=struct.unpack_from('<16sQQQQ',data,at+8)
    if va<=address and address+4<=va+fs:
     count=struct.unpack_from('<I',data,at+64)[0]
     for j in range(count):
      p=at+72+80*j;sec,seg,a,s,o=struct.unpack_from('<16s16sQQI',data,p)
      if a<=address and address+4<=a+s:
       if name.rstrip(b'\0')!=b'__DATA_CONST' or sec.rstrip(b'\0')!=b'__const' or struct.unpack_from('<I',data,p+64)[0]&255:raise ValueError('not expected file-backed constant data')
       if o+address-a!=fo+address-va:raise ValueError('inconsistent file mapping')
       return o+address-a
   at+=n
  raise ValueError('tunable is not file-backed')
 offset=locate(symbols['_user_exception_trace_only']);other=locate(symbols['_user_exception_trace_enabled'])
 if data[offset:offset+4]!=b'\0'*4 or data[other:other+4]!=b'\0'*4:raise ValueError('unexpected control defaults')
 updated=data[:offset]+struct.pack('<I',1)+data[offset+4:]
 if len(updated)!=len(data) or updated[:offset]!=data[:offset] or updated[offset+4:]!=data[offset+4:] or updated[other:other+4]!=b'\0'*4:raise ValueError('unexpected modification')
 dest=OUT/('exception-trace-'+sha(updated)+'.mach')
 if dest.exists() and dest.read_bytes()!=updated:raise ValueError('existing output changed')
 if not dest.exists():dest.write_bytes(updated);dest.chmod(0o400)
 receipt=dict(kernel=str(dest),kernel_sha256=sha(updated),control_sha256=EXPECTED,trace_symbol=hex(symbols['_user_exception_trace_only']),file_offset=hex(offset),before=0,after=1,other_bytes_identical=True,systrace_default=0,hardware_verified=False)
 (OUT/'wmi-carrier-stable-exctrace49.txt').write_text(str(dest)+'\n')
 (OUT/'wmi-carrier-stable-exctrace49.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
if __name__=='__main__':build()
