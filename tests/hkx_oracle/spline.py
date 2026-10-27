"""hkaSplineCompressedAnimation decompressor (hk_2010.2)."""
import struct, math

ROT_SIZE = {0: 4, 1: 5, 2: 6, 3: 3, 4: 2, 5: 16}   # POLAR32 THREECOMP40 THREECOMP48 THREECOMP24 STRAIGHT16 UNCOMPRESSED
ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}
ROT_NAME = {0: 'POLAR32', 1: 'THREECOMP40', 2: 'THREECOMP48', 3: 'THREECOMP24', 4: 'STRAIGHT16', 5: 'UNCOMPRESSED'}
SQRT1_2 = 1 / math.sqrt(2)


class Reader:
    def __init__(self, d, o):
        self.d, self.o = d, o

    def align(self, n):
        self.o = (self.o + n - 1) // n * n

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.o)
        self.o += struct.calcsize('<' + fmt)
        return v


def place(three, missing, sign):
    """Rebuild a unit quaternion (x,y,z,w) from its three smallest components."""
    w = math.sqrt(max(0.0, 1.0 - sum(c * c for c in three)))
    if sign:
        w = -w
    q = list(three)
    q.insert(missing, w)
    return tuple(q)


def decode_quat(kind, b):
    if kind == 1:  # THREECOMP40: 3 x 12 bits, 2 bits missing-index, 1 bit sign
        v = int.from_bytes(b, 'little')
        c = [((v >> (12 * i)) & 0xFFF) for i in range(3)]
        three = [(x - 2047) * (SQRT1_2 / 2047) for x in c]
        return place(three, (v >> 36) & 3, (v >> 38) & 1)
    if kind == 2:  # THREECOMP48: 3 x int16, missing-index split over x/y high bits, sign in z high bit
        x, y, z = struct.unpack('<3H', b)
        missing = ((y >> 14) & 2) | ((x >> 15) & 1)
        three = [((c & 0x7FFF) - 0x3FFF) * (SQRT1_2 / 0x3FFF) for c in (x, y, z)]
        return place(three, missing, z >> 15)
    if kind == 5:
        return struct.unpack('<4f', b)
    raise NotImplementedError(ROT_NAME[kind])


def read_vec_track(r, types, qbits, default):
    """Returns ('spline', degree, knots, ctrl[list of xyz]) or ('const', xyz)."""
    if types & 0xF0:
        n, deg = r.take('HB')
        knots = r.take('%dB' % (n + deg + 2))
        r.align(4)
        ranges = []
        for ax in range(3):
            if types & (0x10 << ax):
                ranges.append(('s',) + r.take('2f'))
            elif types & (1 << ax):
                ranges.append(('c', r.take('f')[0]))
            else:
                ranges.append(('c', default))
        qfmt, qmax = ('B', 255.0) if qbits == 0 else ('H', 65535.0)
        if qbits == 1:
            r.align(2)
        ctrl = []
        for _ in range(n + 1):
            p = []
            for rg in ranges:
                if rg[0] == 's':
                    q, = r.take(qfmt)
                    p.append(rg[1] + (rg[2] - rg[1]) * q / qmax)
                else:
                    p.append(rg[1])
            ctrl.append(tuple(p))
        r.align(4)
        return ('spline', deg, knots, ctrl)
    v = []
    for ax in range(3):
        v.append(r.take('f')[0] if types & (1 << ax) else default)
    r.align(4)
    return ('const', tuple(v))


def read_rot_track(r, types, kind):
    sz, al = ROT_SIZE[kind], ROT_ALIGN[kind]
    if types & 0xF0:
        n, deg = r.take('HB')
        knots = r.take('%dB' % (n + deg + 2))
        r.align(al)
        ctrl = []
        for _ in range(n + 1):
            ctrl.append(decode_quat(kind, r.d[r.o:r.o + sz])); r.o += sz
        r.align(4)
        return ('spline', deg, knots, ctrl)
    if types & 0x0F:
        r.align(al)
        q = decode_quat(kind, r.d[r.o:r.o + sz]); r.o += sz
        r.align(4)
        return ('const', q)
    return ('const', (0.0, 0.0, 0.0, 1.0))


def parse_block(anim, b):
    d = anim['data']
    start = anim['blockOffsets'][b]
    n = anim['nTransformTracks']
    masks = [d[start + 4 * i:start + 4 * i + 4] for i in range(n)]
    r = Reader(d, start + anim['maskAndQuantizationSize'])
    tracks = []
    for q, pt, rt, st in masks:
        pos = read_vec_track(r, pt, q & 3, 0.0)
        rot = read_rot_track(r, rt, (q >> 2) & 0xF)
        scl = read_vec_track(r, st, (q >> 6) & 3, 1.0)
        tracks.append((pos, rot, scl))
    return tracks, r.o - start


def find_span(n, deg, u, knots):
    if u >= knots[n + 1]:
        return n
    lo, hi = deg, n + 1
    mid = (lo + hi) // 2
    while u < knots[mid] or u >= knots[mid + 1]:
        if u < knots[mid]:
            hi = mid
        else:
            lo = mid
        mid = (lo + hi) // 2
    return mid


def eval_spline(track, u, quat=False):
    kind = track[0]
    if kind == 'const':
        return track[1]
    _, deg, knots, ctrl = track
    n = len(ctrl) - 1
    span = find_span(n, deg, u, knots)
    # de Boor
    pts = [list(ctrl[span - deg + j]) for j in range(deg + 1)]
    for rr in range(1, deg + 1):
        for j in range(deg, rr - 1, -1):
            i = span - deg + j
            den = knots[i + deg + 1 - rr] - knots[i]
            a = 0.0 if den == 0 else (u - knots[i]) / den
            pts[j] = [(1 - a) * p0 + a * p1 for p0, p1 in zip(pts[j - 1], pts[j])]
    out = pts[deg]
    if quat:
        l = math.sqrt(sum(c * c for c in out)) or 1.0
        out = [c / l for c in out]
    return tuple(out)


def sample(anim, blocks, frame):
    """Local (pos, quat xyzw, scale) for every track at an integer frame."""
    fpb = anim['maxFramesPerBlock'] - 1
    b = min(frame // fpb, anim['numBlocks'] - 1)
    u = float(frame - b * fpb)
    return [(eval_spline(p, u), eval_spline(q, u, True), eval_spline(s, u))
            for p, q, s in blocks[b]]
