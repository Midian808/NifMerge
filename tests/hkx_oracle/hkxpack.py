"""Minimal reader for Havok 2010.2 binary packfiles (Skyrim LE, 32-bit)."""
import struct, sys


class Section:
    def __init__(self, f, off):
        h = f[off:off + 0x30]
        self.tag = h[:19].split(b'\0')[0].decode()
        (self.start, self.local, self.glob, self.virt,
         self.exports, self.imports, self.end) = struct.unpack_from('<7I', h, 20)


class Packfile:
    def __init__(self, path):
        self.f = open(path, 'rb').read()
        f = self.f
        assert f[:8] == bytes.fromhex('57e0e05710c0c010'), 'not a packfile'
        (self.userTag, self.fileVersion) = struct.unpack_from('<2i', f, 8)
        self.layout = tuple(f[16:20])  # ptrSize, littleEndian, reusePadding, emptyBase
        (self.numSections, self.contentsSec, self.contentsOff,
         self.contentsClsSec, self.contentsClsOff) = struct.unpack_from('<5i', f, 20)
        self.version = f[40:56].split(b'\0')[0].decode()
        self.sections = [Section(f, 0x40 + i * 0x30) for i in range(self.numSections)]
        d = self.sections[2]
        assert d.tag == '__data__'
        self.data = d
        base = d.start
        # local fixups: (src, dst) within data section
        self.ptr = {}
        o = base + d.local
        while o + 8 <= base + d.glob:
            s, t = struct.unpack_from('<2I', f, o); o += 8
            if s != 0xFFFFFFFF:
                self.ptr[s] = t
        # global fixups: (src, dstSection, dst)
        o = base + d.glob
        while o + 12 <= base + d.virt:
            s, sec, t = struct.unpack_from('<3I', f, o); o += 12
            if s != 0xFFFFFFFF:
                assert sec == 2, sec
                self.ptr[s] = t
        # virtual fixups: object offset -> class name
        cn = self.sections[0]
        self.objects = {}
        o = base + d.virt
        while o + 12 <= base + d.exports:
            s, sec, t = struct.unpack_from('<3I', f, o); o += 12
            if s != 0xFFFFFFFF:
                name = f[cn.start + t:].split(b'\0')[0].decode()
                self.objects[s] = name

    # --- primitive readers, offsets relative to data section ---
    def raw(self, off, n):
        return self.f[self.data.start + off:self.data.start + off + n]

    def u(self, fmt, off):
        return struct.unpack_from('<' + fmt, self.f, self.data.start + off)

    def p(self, off):
        """Resolved pointer at off (None if null)."""
        return self.ptr.get(off)

    def string(self, off):
        t = self.p(off)
        if t is None:
            return None
        return self.raw(t, 256).split(b'\0')[0].decode('latin1')

    def array(self, off):
        """hkArray: returns (dataOffset, size)."""
        size, = self.u('I', off + self.layout[0])
        return self.p(off), size

    def find(self, cls):
        return [o for o, c in sorted(self.objects.items()) if c == cls]


if __name__ == '__main__':
    pf = Packfile(sys.argv[1])
    print(pf.version, 'layout', pf.layout, 'sections', [(s.tag, hex(s.start), hex(s.end)) for s in pf.sections])
    for o, c in sorted(pf.objects.items()):
        print(hex(o), c)
