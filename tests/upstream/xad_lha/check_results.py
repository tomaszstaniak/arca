#!/usr/bin/env python3
"""Check each isolated XAD fix, and unchanged output outside its scope."""
from pathlib import Path
import json,re,sys
r=Path(__file__).parent;c=Path(sys.argv[1]);want=json.loads((r/'fixtures/expected.json').read_text())
def load(kind):
 texts=[]
 for key in want:
  assert (c/f'lhax2-{kind}-{key}.rc').read_text().strip()=='0',(kind,key)
  texts.append((c/f'lhax2-{kind}-{key}.txt').read_text(encoding='latin1'))
 s='\n'.join(texts);out={};name=None
 for line in s.splitlines():
  if line.startswith('ARCHIVE '):
   assert line.endswith(' error=0'),line
   name=line.split('/')[-1].split('.lha')[0]
  elif line.startswith('ENTRY '):
   m=re.fullmatch(r'ENTRY (.*?) type=(\w+) prot=([0-9a-f]+) comment=(.*)',line);assert m,line
   assert name not in out,name
   out[name]={'name':m[1],'type':m[2],'prot':int(m[3],16),'comment':m[4]}
 assert set(out)==set(want),(kind,set(want)-set(out))
 return out
base=load('baseline');allfixed={}
for kind in ['spaces','hold','directories','protection']:
 out=load(kind)
 keys=[k for k in want if (kind=='spaces' and k.startswith('space-')) or (kind=='hold' and k.startswith('hold-')) or (kind=='directories' and k.startswith('dir-')) or (kind=='protection' and k.startswith(('unix-','dos-')))]
 changed=[]
 for k in want:
  if k in keys:
   for field,val in want[k].items():assert out[k][field]==val,(kind,k,field,out[k],val)
   if out[k]!=base[k]:changed.append(k)
  else:assert out[k]==base[k],('unexpected change',kind,k,base[k],out[k])
 assert changed,('no regression reproduced',kind)
 print(kind,':',len(keys),'target cases pass;',len(want)-len(keys),'controls unchanged; fixes',','.join(changed))
 allfixed[kind]={'targets':keys,'changed':changed}
(r/'results-summary.json').write_text(json.dumps(allfixed,indent=2)+'\n')
print('PASS')
