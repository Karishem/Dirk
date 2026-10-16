# Review point 6 e2e (2026-09-26): reads run.sh's log, screenshots and the
# final game.cfg. Checks that game.cfg is rewritten when a setting really
# changes and left alone when the script re-sets unchanged values, and that
# the scanline overlay still appears/disappears on F12.
import struct, sys, zlib

def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and interlace == 0
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, prev, out, i = zlib.decompress(idat), w * bpp, bytearray(w * bpp), [], 0
    for _ in range(h):
        f, row = raw[i], bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: row[x] = (row[x] + a) & 255
            elif f == 2: row[x] = (row[x] + b) & 255
            elif f == 3: row[x] = (row[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(row); prev = row
    return out, w, h, bpp

def brightness(path):
    rows, w, h, bpp = read_png(path)
    tot = 0
    for r in rows:
        tot += sum(r[x] + r[x + 1] + r[x + 2] for x in range(0, w * bpp, bpp))
    return tot / (w * h * 3)

logp, shots, cfg = sys.argv[1], sys.argv[2], sys.argv[3]
mt = {}
for line in open(logp):
    p = line.split()
    if line.startswith('SHIM t=') and len(p) >= 4 and p[2] != '-':
        mt[p[2]] = int(p[3].split('=')[1])
ok = True
def check(cond, what):
    global ok
    ok &= bool(cond)
    print(('PASS ' if cond else 'FAIL ') + what)

check(mt.get('vol_plus') != mt.get('after_plus'), 'volume +10 (real change): game.cfg rewritten')
check(mt.get('same_values') == mt.get('after_same'), 'script re-sets all 6 settings to unchanged values: game.cfg left alone')
b = {k: brightness('%s/%s.png' % (shots, k)) for k in ('shot_off', 'shot_on', 'shot_same', 'shot_off2')}
print('brightness', {k: round(v, 2) for k, v in b.items()})
check(b['shot_on'] < b['shot_off'] * 0.97, 'F12: scanlines appear (picture darker)')
check(abs(b['shot_same'] - b['shot_on']) < 0.01 * b['shot_on'], 'unchanged re-set: scanlines still shown')
check(abs(b['shot_off2'] - b['shot_off']) < 0.01 * b['shot_off'], 'F12 again: scanlines gone')
final = dict(l.split(' = ', 1) for l in open(cfg).read().splitlines() if l.startswith('dirk_'))
check(final.get('dirk_VolumeVldp') == '60' and final.get('dirk_VolumeNonVldp') == '60' and final.get('dirk_Scanlines') == '0',
      'final game.cfg: volumes 60/60, scanlines 0 (%s)' % final)
check(sum(1 for l in open(cfg) if l.startswith('dip_Test')) == 40, "final game.cfg: the game's own 40 lines kept")
print('ALL PASS' if ok else 'SOME FAILED'); sys.exit(0 if ok else 1)
