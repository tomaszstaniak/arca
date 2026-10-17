#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Host-side checks of the macOS build of unarc-7z against 7zz (7z) and
# The Unarchiver's lsar/unar (RAR; Homebrew's 7zz has no RAR codecs).
# usage: python3 -I host_test.py   (after build.sh host)
import hashlib, json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, 'build', '7z-backend')
U = os.path.join(OUT, 'host', 'bin', 'unarc-7z')
CORPUS = os.path.join(ROOT, 'build', 'corpus')
TEST = os.path.join(OUT, 'test')
SZ = '/opt/homebrew/bin/7zz'
LSAR = '/opt/homebrew/bin/lsar'
UNAR = '/opt/homebrew/bin/unar'
LIBARCHIVE_TEST = os.path.join(ROOT, 'deps', 'libarchive', 'libarchive', 'test')

fails = 0
def check(cond, what):
    global fails
    print(('PASS ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1

# Listings must not depend on the host's time zone (checked below); TZ=UTC
# only keeps the saved reference listings comparable with older ones.
ENV = dict(os.environ, TZ='UTC')

def run(args, password=None):
    inp = (password + '\n').encode() if password is not None else b''
    return subprocess.run([U] + args, input=inp, stdout=subprocess.PIPE, check=False, env=ENV).stdout

def save_listing(path, password=None):
    """Reference listings for comparing the AROS build's output."""
    d = os.path.join(OUT, 'expected')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, os.path.basename(path) + '.list'), 'wb') as f:
        f.write(run(['list', path], password))

def parse_list(out):
    arc, entries, res = None, [], None
    for line in out.decode('utf-8').splitlines():
        f = line.split('\t')
        if f[0] == 'A':
            arc = f
        elif f[0] == 'E':
            entries.append({'index': int(f[1]), 'dir': f[2] == '1',
                            'size': None if f[3] == '-' else int(f[3]),
                            'enc': f[7] == '1', 'name': bytes.fromhex(f[8]).decode('utf-8')})
        elif f[0] == 'R':
            res = (int(f[1]), f[2])
    return arc, entries, res

def parse_multi(out):
    """D/F records then R; returns ({index: (data or None, F code, F message)}, R code, R message)
    and checks the framing: every D is followed by its F, R is the last line."""
    ents = {}
    pending = {}
    while True:
        nl = out.index(b'\n')
        f = out[:nl].split(b'\t')
        if f[0] == b'H':
            assert int(f[1]) not in ents and int(f[1]) not in pending, 'H after its D'
            out = out[nl + 1:]
            continue
        if f[0] == b'D':
            n = int(f[2])
            pending[int(f[1])] = out[nl + 1:nl + 1 + n]
            out = out[nl + 1 + n:]
            continue
        if f[0] == b'F':
            k = int(f[1])
            ents[k] = (pending.pop(k, None), int(f[2]), f[3].decode('utf-8'))
            out = out[nl + 1:]
            continue
        assert f[0] == b'R' and nl == len(out) - 1, 'bad framing'
        assert not pending, 'D without F'
        return ents, int(f[1]), f[2].decode('utf-8')

def parse_extract(out):
    """One entry: (data or None, code, message) from its F record and R."""
    ents, code, msg = parse_multi(out)
    assert len(ents) <= 1, 'more than one entry'
    data = next(iter(ents.values()))[0] if ents else None
    return data, code, msg

def sha(b):
    return hashlib.sha256(b).hexdigest()

def tree_hashes(d):
    r = {}
    for base, _, files in os.walk(d):
        for fn in files:
            p = os.path.join(base, fn)
            if os.path.islink(p):
                continue
            r[os.path.relpath(p, d)] = sha(open(p, 'rb').read())
    return r

# ---------------------------------------------------------------- 7z, plain
def test_7z(path, password=None):
    arc, ents, res = parse_list(run(['list', path], password))
    check(res and res[0] == 0, f'{os.path.basename(path)}: list R 0 ({len(ents)} entries)')
    slt = subprocess.run([SZ, 'l', '-slt', '-p' + (password or '')] + [path],
                         stdout=subprocess.PIPE, check=True).stdout.decode()
    ref = {}
    for block in slt.split('\n\n'):
        kv = dict(l.split(' = ', 1) for l in block.splitlines() if ' = ' in l)
        if 'Path' in kv and 'Size' in kv and 'Attributes' in kv:
            ref[kv['Path']] = (kv['Attributes'].startswith('D'), int(kv['Size'] or 0))
    mine = {e['name']: (e['dir'], e['size'] or 0) for e in ents}
    check(mine == ref, f'{os.path.basename(path)}: names/dir/size match 7zz l ({len(ref)})')
    ok = 0
    files = [e for e in ents if not e['dir']]
    for e in files:
        data, code, msg = parse_extract(run(['extract', path, str(e['index'])], password))
        refdata = subprocess.run([SZ, 'e', '-so', '-p' + (password or ''), path, e['name']],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout
        if code == 0 and data == refdata:
            ok += 1
        else:
            print(f'   mismatch {e["name"]}: code {code} {msg}')
    check(ok == len(files), f'{os.path.basename(path)}: {ok}/{len(files)} entries extract identical to 7zz e -so')

# ---------------------------------------------------------------- RAR sets
def test_rar(path, password=None):
    name = os.path.basename(path)
    arc, ents, res = parse_list(run(['list', path], password))
    check(res and res[0] == 0, f'{name}: list R 0, A={arc[1:] if arc else None}')
    lj = json.loads(subprocess.run([LSAR, '-j'] + (['-p', password] if password else []) + [path],
                                   stdout=subprocess.PIPE, check=True).stdout)
    ref = {}
    for c in lj['lsarContents']:
        n = c['XADFileName']
        isdir = bool(c.get('XADIsDirectory', 0))
        # lsar lists a file split over volumes once; so does 7-Zip.
        ref[n] = (isdir, c.get('XADFileSize', 0) if not isdir else 0)
    mine = {e['name']: (e['dir'], e['size'] if not e['dir'] else 0) for e in ents}
    check(mine == ref, f'{name}: names/dir/size match lsar ({len(ref)} entries)')
    if mine != ref:
        for k in sorted(set(mine) | set(ref)):
            if mine.get(k) != ref.get(k):
                print('   ', repr(k), 'unarc-7z', mine.get(k), 'lsar', ref.get(k))
    tmp = tempfile.mkdtemp(dir=TEST)
    try:
        subprocess.run([UNAR, '-q', '-f', '-o', tmp] + (['-p', password] if password else []) + [path],
                       check=True, stdout=subprocess.DEVNULL)
        sub = os.listdir(tmp)
        refdir = os.path.join(tmp, sub[0]) if len(sub) == 1 and os.path.isdir(os.path.join(tmp, sub[0])) \
            and not any(e['name'].startswith(sub[0] + '/') or e['name'] == sub[0] for e in ents) else tmp
        refh = tree_hashes(refdir)
        ok = n = 0
        for e in ents:
            if e['dir'] or e['name'] not in refh:
                continue
            n += 1
            data, code, msg = parse_extract(run(['extract', path, str(e['index'])], password))
            if code == 0 and sha(data) == refh[e['name']]:
                ok += 1
            else:
                print(f'   mismatch {e["name"]}: code {code} {msg}')
        check(n > 0 and ok == n, f'{name}: {ok}/{n} regular files extract identical to unar')
    finally:
        shutil.rmtree(tmp)

def expect(args, password, code, what):
    out = run(args, password)
    if args[0] == 'list':
        _, _, res = parse_list(out)
    else:
        _, c, m = parse_extract(out)
        res = (c, m)
    check(res is not None and res[0] == code, f'{what}: R {res[0] if res else None} ({res[1] if res else ""}), want {code}')

def main():
    os.makedirs(TEST, exist_ok=True)
    # Encrypted 7z test archives.
    src = os.path.join(TEST, 'src')
    if not os.path.isdir(src):
        os.makedirs(os.path.join(src, 'sub'))
        open(os.path.join(src, 'a.txt'), 'w').write('hello encrypted world\n')
        open(os.path.join(src, 'sub', 'rand.bin'), 'wb').write(os.urandom(300000))
        open(os.path.join(src, 'sub', 'seq.txt'), 'w').write(''.join(f'{i}\n' for i in range(1, 50001)))
    enc, hdr = os.path.join(TEST, 'enc.7z'), os.path.join(TEST, 'hdr.7z')
    for p, extra in ((enc, []), (hdr, ['-mhe=on'])):
        if not os.path.exists(p):
            subprocess.run([SZ, 'a', '-psecret'] + extra + [p, 'a.txt', 'sub'], cwd=src, check=True,
                           stdout=subprocess.DEVNULL)

    test_7z(os.path.join(CORPUS, 'tree.7z'))
    for f in ('r5.part01.rar', 'r4.part1.rar', 'old.rar'):
        test_rar(os.path.join(CORPUS, f))

    # 7z with encrypted data, plain headers: listing needs no password.
    expect(['list', enc], None, 0, 'enc.7z list, no password')
    expect(['extract', enc, '1'], None, 1, 'enc.7z extract, no password')
    expect(['extract', enc, '1'], 'wrong', 2, 'enc.7z extract, wrong password')
    test_7z(enc, 'secret')
    # 7z with encrypted headers.
    expect(['list', hdr], None, 1, 'hdr.7z list, no password')
    expect(['list', hdr], 'wrong', 2, 'hdr.7z list, wrong password')
    test_7z(hdr, 'secret')

    # Encrypted RAR4/RAR5 from libarchive's test suite (password "password";
    # in the non-solid ones d.txt uses "password2", a.txt and c.txt are plain).
    rdir = os.path.join(TEST, 'rar-enc')
    os.makedirs(rdir, exist_ok=True)
    for v in ('rar4', 'rar5'):
        for kind in ('encrypted', 'encrypted_filenames', 'solid_encrypted', 'solid_encrypted_filenames'):
            f = os.path.join(rdir, f'{v}_{kind}.rar')
            if not os.path.exists(f):
                subprocess.run(['uudecode', '-o', f, os.path.join(LIBARCHIVE_TEST, f'test_read_format_{v}_{kind}.rar.uu')], check=True)
            hdrenc = kind.endswith('filenames')
            if hdrenc:
                expect(['list', f], None, 1, f'{v}_{kind} list, no password')
                expect(['list', f], 'wrong', 2, f'{v}_{kind} list, wrong password')
            else:
                expect(['list', f], None, 0, f'{v}_{kind} list, no password')
            expect(['extract', f, '1'], None, 1, f'{v}_{kind} extract b.txt, no password')
            expect(['extract', f, '1'], 'wrong', 2, f'{v}_{kind} extract b.txt, wrong password')
            data, code, _ = parse_extract(run(['extract', f, '1'], 'password'))
            check(code == 0 and data == b'This is from b.txt', f'{v}_{kind} extract b.txt with password')
            if not kind.startswith('solid'):
                data, code, _ = parse_extract(run(['extract', f, '3'], 'password2' if not hdrenc else 'password'))
                check(code == 0 and data == b'This is from d.txt', f'{v}_{kind} extract d.txt with its password')

    for f in ('tree.7z', 'r5.part01.rar', 'r4.part1.rar', 'old.rar', 'gap.part1.rar'):
        save_listing(os.path.join(CORPUS, f))
    save_listing(enc)
    save_listing(hdr, 'secret')

    # One run for many entries ("all", or a list), as aros-unarc extracts: the
    # data must equal one run per entry, solid archives included.
    def one_by_one(path, password=None):
        _, ents, _ = parse_list(run(['list', path], password))
        r = {}
        for e in ents:
            data, code, _ = parse_extract(run(['extract', path, str(e['index'])], password))
            r[e['index']] = (data, code)
        return ents, r
    for path, pw in ((os.path.join(CORPUS, 'tree.7z'), None), (enc, 'secret'), (hdr, 'secret'),
                     (os.path.join(CORPUS, 'r5.part01.rar'), None),
                     (os.path.join(CORPUS, 'r4.part1.rar'), None),
                     (os.path.join(rdir, 'rar5_solid_encrypted.rar'), 'password'),
                     (os.path.join(rdir, 'rar4_solid_encrypted.rar'), 'password')):
        ents, single = one_by_one(path, pw)
        multi, code, msg = parse_multi(run(['extract', path, 'all'], pw))
        same = set(multi) == set(single) and all(multi[k][0] == single[k][0] and multi[k][1] == single[k][1]
                                                 for k in multi)
        check(same, f'{os.path.basename(path)}: extract all = {len(ents)} single runs (R {code} {msg})')
        if len(ents) >= 2:
            pick = ','.join(str(e['index']) for e in reversed(ents[:2]))
            sub, code, _ = parse_multi(run(['extract', path, pick], pw))
            check(sorted(sub) == sorted(e['index'] for e in ents[:2]) and
                  all(sub[k][0] == single[k][0] for k in sub), f'{os.path.basename(path)}: extract {pick} (unsorted list)')

    # Hex path and the password from a file instead of stdin.
    hx = hdr.encode().hex()
    a = run(['list', hdr], 'secret')
    pwf = os.path.join(TEST, 'pw.txt')
    open(pwf, 'w').write('secret\n')
    check(run(['listx', hx], 'secret') == a, 'hdr.7z listx (hex path) = list')
    check(run(['-p', pwf, 'listx', hx]) == a, 'hdr.7z -p file: same listing, stdin unused')
    check(run(['-p', pwf, 'extractx', hx, 'all']) == run(['extract', hdr, 'all'], 'secret'),
          'hdr.7z -p file extractx all = extract all')
    idxf = os.path.join(TEST, 'idx.txt')
    open(idxf, 'w').write('2\n0\n')
    check(run(['extract', hdr, '@' + idxf], 'secret') == run(['extract', hdr, '0,2'], 'secret'),
          'hdr.7z extract @file = extract 0,2')
    expect(['listx', 'zz'], None, 9, 'listx with bad hex')
    expect(['extract', os.path.join(CORPUS, 'tree.7z'), '9999'], None, 9, 'extract index out of range')

    # Missing middle volume: gap.part1, part2, part4 (no part3).
    expect(['list', os.path.join(CORPUS, 'gap.part1.rar')], None, 4, 'gap.part1.rar list (part3 missing)')
    expect(['list', os.path.join(CORPUS, 'tree.tar.gz')], None, 5, 'tree.tar.gz list (format not in this build)')

    # Version and protocol number, which the AROS engine checks.
    out = subprocess.run([U, 'version'], stdout=subprocess.PIPE, check=False).stdout.decode()
    check(out == 'V\t2\t7-Zip 26.04\nR\t0\tok\n', f'version: {out!r}')

    # Times. 7z and RAR 5 store UTC; RAR 1.5-4 stores local time, which must
    # come out as stored (seconds as if UTC), whatever zone the process has.
    for f in ('old.rar', 'r5.part01.rar', 'tree.7z'):
        path = os.path.join(CORPUS, f)
        outs = {tz: subprocess.run([U, 'list', path], stdout=subprocess.PIPE, check=False,
                                   env=dict(os.environ, TZ=tz)).stdout
                for tz in ('UTC', 'America/New_York', 'Asia/Tokyo')}
        check(len(set(outs.values())) == 1, f'{f}: listing the same in UTC, New York and Tokyo')
    import calendar, struct
    raw = open(os.path.join(CORPUS, 'old.rar'), 'rb').read()
    pos = 7 + struct.unpack_from('<H', raw, 7 + 5)[0]          # past the main header
    assert raw[pos + 2] == 0x74
    dos = struct.unpack_from('<I', raw, pos + 20)[0]
    d, t = dos >> 16, dos & 0xFFFF
    stored = calendar.timegm((1980 + (d >> 9), (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2))
    for tz in ('America/New_York', 'Asia/Tokyo'):
        o = subprocess.run([U, 'list', os.path.join(CORPUS, 'old.rar')], stdout=subprocess.PIPE,
                           check=False, env=dict(os.environ, TZ=tz)).stdout.decode()
        first = [l.split('\t') for l in o.splitlines() if l.startswith('E\t0\t')][0]
        # The DOS field has 2-second steps; RAR's extended time adds the rest.
        check(0 <= int(first[5]) - stored < 2,
              f'old.rar (RAR 4) in {tz}: first entry time {first[5]} is the stored local time {stored} (+ext)')

    # ZIP: ZipCrypto and WinZip AES (libarchive in aros-unarc has no AES), plain,
    # and names: a CP437 name (no UTF-8 flag) must come out as its raw bytes,
    # a flagged one as UTF-8.
    zdir = os.path.join(TEST, 'zip')
    os.makedirs(zdir, exist_ok=True)
    zips = {}
    for name, extra in (('zc.zip', ['-psecret', '-mem=ZipCrypto']), ('za.zip', ['-psecret', '-mem=AES256']),
                        ('zp.zip', [])):
        p = os.path.join(zdir, name)
        if not os.path.exists(p):
            subprocess.run([SZ, 'a', '-tzip'] + extra + [p, 'a.txt', 'sub'], cwd=src, check=True,
                           stdout=subprocess.DEVNULL)
        zips[name] = p
    test_7z(zips['zc.zip'], 'secret')
    test_7z(zips['za.zip'], 'secret')
    test_7z(zips['zp.zip'])
    for name in ('zc.zip', 'za.zip'):
        _, ents, _ = parse_list(run(['list', zips[name]]))
        i = str([e for e in ents if not e['dir']][0]['index'])
        expect(['extract', zips[name], i], None, 1, f'{name} extract, no password')
        expect(['extract', zips[name], i], 'wrong', 2, f'{name} extract, wrong password')
    import zipfile
    zn = os.path.join(zdir, 'names.zip')
    with zipfile.ZipFile(zn, 'w') as z:
        z.writestr('GrXn.txt', b'cp437 name\n')                # patched to CP437 below: no flag
        z.writestr('\u20acuro.txt', b'utf-8 name\n')          # Python sets the UTF-8 flag
    raw = open(zn, 'rb').read()
    open(zn, 'wb').write(raw.replace(b'GrXn.txt', b'Gr\x81n.txt'))   # both headers; no CRC covers names
    out = run(['list', zn])
    names = [bytes.fromhex(l.split('\t')[8]) for l in out.decode().splitlines() if l.startswith('E\t')]
    # 7-Zip on AROS (and here) decodes a name without the flag as UTF-8 and
    # keeps bytes that are not UTF-8 as they were; aros-unarc then reads such
    # bytes as CP437, as it does for libarchive.
    check(names == [b'Gr\x81n.txt', '\u20acuro.txt'.encode()], f'names.zip: raw CP437 and UTF-8 names {names}')
    arc = [l for l in out.decode().splitlines() if l.startswith('A\t')]
    check(arc and arc[0].split('\t')[1] == 'zip', f'names.zip: A record {arc}')

    print(f'{fails} failure(s)')
    sys.exit(1 if fails else 0)

main()
