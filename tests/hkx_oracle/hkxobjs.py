"""Field readers for Skyrim animation classes (hk_2010.2.0-r1): LE 32-bit and SSE 64-bit layouts."""

# Member offsets per pointer size. 32-bit: hkReferencedObject 8, hkArray 12, pointer 4.
# 64-bit: hkReferencedObject 16, hkArray 16, pointer 8.
LAYOUT = {
    4: dict(
        bone=8, annTrack=16, ann=8, annText=4, annArr=4,
        skel=dict(name=8, parents=12, bones=24, pose=36, refFloats=48, floatSlots=60, localFrames=72),
        anim=dict(type=8, extractedMotion=24, annotations=28, spline=40,
                  blockOffsets=68, floatBlockOffsets=80, transformOffsets=92, floatOffsets=104,
                  data=116, endian=128),
        bind=dict(skelName=8, animation=12, trackToBone=16, floatToSlot=28, blendHint=40),
        ref=dict(up=16, forward=32, duration=48, samples=52)),
    8: dict(
        bone=16, annTrack=24, ann=16, annText=8, annArr=8,
        skel=dict(name=16, parents=24, bones=40, pose=56, refFloats=72, floatSlots=88, localFrames=104),
        anim=dict(type=16, extractedMotion=32, annotations=40, spline=56,
                  blockOffsets=88, floatBlockOffsets=104, transformOffsets=120, floatOffsets=136,
                  data=152, endian=168),
        bind=dict(skelName=16, animation=24, trackToBone=32, floatToSlot=48, blendHint=64),
        ref=dict(up=16, forward=32, duration=48, samples=56)),
}


def L(pf):
    return LAYOUT[pf.layout[0]]


def read_skeleton(pf, o):
    l = L(pf); k = l['skel']
    name = pf.string(o + k['name'])
    pa, pn = pf.array(o + k['parents'])
    parents = list(pf.u('%dh' % pn, pa)) if pn else []
    ba, bn = pf.array(o + k['bones'])
    bones = [(pf.string(ba + l['bone'] * i), pf.u('B', ba + l['bone'] * i + pf.layout[0])[0]) for i in range(bn)]
    ra, rn = pf.array(o + k['pose'])
    pose = [pf.u('12f', ra + 48 * i) for i in range(rn)]  # t(4) q(4) s(4)
    _, nfloats = pf.array(o + k['refFloats'])
    _, nslots = pf.array(o + k['floatSlots'])
    _, nframes = pf.array(o + k['localFrames'])
    return dict(name=name, parents=parents, bones=bones, pose=pose,
                nRefFloats=nfloats, nFloatSlots=nslots, nLocalFrames=nframes)


def read_annotations(pf, o):
    l = L(pf)
    aa, an = pf.array(o)
    tracks = []
    for i in range(an):
        t = aa + l['annTrack'] * i
        name = pf.string(t)
        la, ln = pf.array(t + l['annArr'])
        notes = [(pf.u('f', la + l['ann'] * k)[0], pf.string(la + l['ann'] * k + l['annText'])) for k in range(ln)]
        tracks.append((name, notes))
    return tracks


def read_animation(pf, o):
    k = L(pf)['anim']
    typ, dur, ntt, nft = pf.u('ifii', o + k['type'])
    a = dict(type=typ, duration=dur, nTransformTracks=ntt, nFloatTracks=nft,
             extractedMotion=pf.p(o + k['extractedMotion']),
             annotations=read_annotations(pf, o + k['annotations']))
    if pf.objects[o] == 'hkaSplineCompressedAnimation':
        (a['numFrames'], a['numBlocks'], a['maxFramesPerBlock'], a['maskAndQuantizationSize'],
         a['blockDuration'], a['blockInverseDuration'], a['frameDuration']) = pf.u('4i3f', o + k['spline'])
        for name in ('blockOffsets', 'floatBlockOffsets', 'transformOffsets', 'floatOffsets'):
            da, dn = pf.array(o + k[name])
            a[name] = list(pf.u('%dI' % dn, da)) if dn else []
        da, dn = pf.array(o + k['data'])
        a['data'] = pf.raw(da, dn)
        a['endian'], = pf.u('i', o + k['endian'])
    return a


def read_binding(pf, o):
    k = L(pf)['bind']
    ta, tn = pf.array(o + k['trackToBone'])
    fa, fn = pf.array(o + k['floatToSlot'])
    return dict(skeletonName=pf.string(o + k['skelName']), animation=pf.p(o + k['animation']),
                trackToBone=list(pf.u('%dh' % tn, ta)) if tn else [],
                floatToSlot=list(pf.u('%dh' % fn, fa)) if fn else [],
                blendHint=pf.u('b', o + k['blendHint'])[0])


def read_ref_frame(pf, o):
    k = L(pf)['ref']
    up = pf.u('4f', o + k['up'])
    fwd = pf.u('4f', o + k['forward'])
    dur, = pf.u('f', o + k['duration'])
    sa, sn = pf.array(o + k['samples'])
    samples = [pf.u('4f', sa + 16 * i) for i in range(sn)]
    return dict(up=up, forward=fwd, duration=dur, samples=samples)


def pointer_fields(pf, o, size):
    """Offsets inside [o, o+size) that the file's fixup tables say hold pointers."""
    return sorted(s - o for s in pf.ptr if o <= s < o + size)
