"""Per frame: share of the list area that is one flat colour (blank) and a
hash of the top rows, to see blanking and jumps. Usage: blank.py dir x y w h"""
import sys, os, hashlib
d, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
def ppm(p):
    b = open(p, "rb").read()
    parts = b.split(b"\n", 3)
    W, H = map(int, parts[1].split()); data = parts[3]
    return W, H, data
fr = sorted(f for f in os.listdir(d) if f.endswith(".ppm"))
times = [float(t) for t in open(os.path.join(d, "times.txt"))]
prev = None
for i, f in enumerate(fr):
    W, H, data = ppm(os.path.join(d, f))
    from collections import Counter
    c = Counter(); top = bytearray()
    for yy in range(y, y + h, 2):
        row = data[(yy * W + x) * 3:(yy * W + x + w) * 3]
        c.update(row[k:k+3] for k in range(0, len(row), 6))
        if yy < y + 60: top += row
    common = c.most_common(1)[0][1] / sum(c.values())
    th = hashlib.md5(top).hexdigest()[:6]
    mark = " <TOP CHANGED" if prev and th != prev else ""
    prev = th
    print(f"{times[i]:6.2f} flat {common:.2f} top {th}{mark}")
