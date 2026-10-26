#!/usr/bin/env python3
"""Compare two tz.script runs (tests/guest/make_review_cases.py), one at each
time-zone offset: per archive and engine the return codes, how far each
entry's date moved between the runs, and where the two engines disagree.

    tz_compare.py tz-a.txt tz-b.txt <date of run a> <date of run b>

Drawers dated on the run's own day were made by the extraction and are left
out. UTC-stored times (7z, RAR 5, ZIP with a UTC extra field) must move by the
offset, local ones (RAR 4, DOS-only ZIP) must not.
"""
import sys, datetime as D
def parse(p, today):
    sec={}; cur=None; hdr=None
    for l in open(p, encoding='latin-1'):
        l=l.rstrip('\n')
        if l.startswith('gmt offset'): hdr=l; continue
        if l.startswith('== '): cur=l[3:].split(' (')[0]; sec[cur]={'rc':None,'e':{}}; continue
        if l.startswith('RC='): sec[cur]['rc']=l[3:]; continue
        if len(l)>21 and l[4]=='-' and l[10]==' ':
            d,path=l[:19],l[21:]
            if d.startswith(today) and path.endswith('/'): continue
            sec[cur]['e'][path]=D.datetime.strptime(d,'%Y-%m-%d %H:%M:%S')
    return hdr,sec
pa,pb,ta,tb=sys.argv[1:5]
ha,a=parse(pa,ta); hb,b=parse(pb,tb)
print(pa.split('/')[-3:], ha, '|', hb)
for k in a:
    ea,eb=a[k]['e'],b[k]['e']
    sh=sorted({(eb[p]-ea[p]).total_seconds()/3600 for p in ea if p in eb})
    print(f"{k:28s} rc {a[k]['rc']}/{b[k]['rc']} files {len(ea)}/{len(eb)} shift(h) {sh}")
# engines agree
for s,n in ((a,'a'),(b,'b')):
    for arc in sorted({k.rsplit(' ',1)[0] for k in s}):
        l,z=s.get(arc+' libarchive'),s.get(arc+' 7-zip')
        if l and z:
            diff=[p for p in l['e'] if p in z['e'] and l['e'][p]!=z['e'][p]]
            print(f"  {n} {arc}: engines differ on {len(diff)} of {len(l['e'])}", diff[:3])
