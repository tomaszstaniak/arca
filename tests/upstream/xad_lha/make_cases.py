#!/usr/bin/env python3
"""Small stored LHA archives for XAD level-1/2 metadata regressions."""
from pathlib import Path
import struct,json
OUT=Path(__file__).with_name('fixtures'); OUT.mkdir(exist_ok=True)
def crc16(data):
 c=0
 for b in data:
  c^=b
  for _ in range(8):c=(c>>1)^ (0xa001 if c&1 else 0)
 return c
def entry(level,name=b'file',attr=0,osid=b'A',ext=(),method=b'-lh0-',data=b'x'):
 ext=list(ext)
 if level==2:ext.insert(0,(1,name))
 sizes=[len(p)+3 for t,p in ext]
 chain=b''.join(bytes([t])+p+struct.pack('<H',sizes[i+1] if i+1<len(sizes) else 0) for i,(t,p) in enumerate(ext))
 h=bytearray(21)
 h[2:7]=method;h[7:11]=struct.pack('<I',len(data)+(len(chain) if level==1 else 0));h[11:15]=struct.pack('<I',len(data));h[15:19]=struct.pack('<I', 0x1ece6000 if level==1 else 803217600);h[19]=attr;h[20]=level
 if level==1:
  h+=bytes([len(name)])+name+struct.pack('<H',crc16(data))+osid+struct.pack('<H',sizes[0] if sizes else 0)
  h[0]=len(h)-2;h[1]=sum(h[2:])&255
 else:
  h+=struct.pack('<H',crc16(data))+osid+struct.pack('<H',sizes[0] if sizes else 0)
  h[0:2]=struct.pack('<H',len(h)+len(chain))
 return bytes(h)+chain+data
cases={}
def put(n,entries,expected):
 (OUT/(n+'.lha')).write_bytes(b''.join(entries)+b'\0');cases[n]=expected
for level in [1,2]:
 put(f'space-l{level}',[entry(level,name=b'space name')],{'name':'space name','type':'file','prot':0})
 put(f'hold-l{level}',[entry(level,attr=0x80)],{'name':'file','type':'file','prot':128})
 put(f'plain-l{level}',[entry(level,attr=0x65)],{'name':'file','type':'file','prot':101})
 put(f'dir-l{level}',[entry(level,name=b'\0folder comment' if level==1 else b'',method=b'-lhd-',data=b'',ext=[(2,b'docs\xff')])],{'name':'docs','type':'dir','comment':'folder comment' if level==1 else ''})
 put(f'unix-l{level}',[entry(level,ext=[(0x50,struct.pack('<H',0o100644))])],{'name':'file','prot':0x8802})
 put(f'dos-l{level}',[entry(level,ext=[(0x40,struct.pack('<H',1))])],{'name':'file','prot':0x15})
 put(f'unix-dos-l{level}',[entry(level,ext=[(0x50,struct.pack('<H',0o100644)),(0x40,struct.pack('<H',1))])],{'name':'file','prot':0x15})
 put(f'dos-unix-l{level}',[entry(level,ext=[(0x40,struct.pack('<H',1)),(0x50,struct.pack('<H',0o100644))])],{'name':'file','prot':0x8802})
put('dir-named-l1',[entry(1,name=b'docs',method=b'-lhd-',data=b'')],{'name':'docs','type':'dir','prot':0})
put('dir-named-l2',[entry(2,name=b'docs',method=b'-lhd-',data=b'')],{'name':'docs','type':'dir','prot':0})
(OUT/'expected.json').write_text(json.dumps(cases,indent=2)+'\n')
print(len(cases),'fixtures')
