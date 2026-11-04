using System;
using System.IO;

namespace NifMerge
{
    // Skyrim's DDS files often stop their mip chain before 1x1 (e.g. 11 levels for a 2048 map, which needs
    // 12). Unity refuses such files ("Invalid mipCount"). A copy with the chain completed imports: every level
    // the file has is kept byte for byte, and each missing level (the smallest, at most a few blocks) repeats
    // the last block (or pixel) of the smallest level present.
    public static class NifMergeDds
    {
        const int HeaderSize = 128, Dx10HeaderSize = 20;
        const uint FlagMipMapCount = 0x20000, CapsComplex = 0x8, CapsMipMap = 0x400000, Caps2Cubemap = 0x200;

        // Writes dst with a complete mip chain. Returns null on success, else why the file cannot be completed.
        public static string CompleteMipChain(string src, string dst)
        {
            byte[] f = File.ReadAllBytes(src);
            if (f.Length < HeaderSize || f[0] != 'D' || f[1] != 'D' || f[2] != 'S' || f[3] != ' ') return "not a DDS file";
            uint flags = U32(f, 8), height = U32(f, 12), width = U32(f, 16), mips = Math.Max(1u, U32(f, 28));
            uint pfFlags = U32(f, 80), fourCC = U32(f, 84), bitCount = U32(f, 88), caps = U32(f, 108), caps2 = U32(f, 112);
            if ((caps2 & Caps2Cubemap) != 0) return "a cube map";
            bool dx10 = fourCC == FourCC("DX10");
            int dataStart = HeaderSize + (dx10 ? Dx10HeaderSize : 0);
            if (dx10 && (f.Length < dataStart || U32(f, 140) > 1)) return "a texture array";

            // Bytes per unit: a 4x4 block for block-compressed formats, a pixel otherwise.
            int unit;
            bool blocks = true;
            if (dx10)
            {
                uint dxgi = U32(f, 128);
                if (dxgi >= 70 && dxgi <= 72 || dxgi >= 79 && dxgi <= 81) unit = 8;                 // BC1, BC4
                else if (dxgi >= 73 && dxgi <= 78 || dxgi >= 82 && dxgi <= 84 || dxgi >= 94 && dxgi <= 99) unit = 16; // BC2, BC3, BC5, BC6H, BC7
                else if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 91) { unit = 4; blocks = false; } // RGBA8 / BGRA8
                else return $"DXGI format {dxgi}";
            }
            else if (fourCC == FourCC("DXT1") || fourCC == FourCC("ATI1") || fourCC == FourCC("BC4U") || fourCC == FourCC("BC4S")) unit = 8;
            else if (fourCC == FourCC("DXT3") || fourCC == FourCC("DXT5") || fourCC == FourCC("ATI2") || fourCC == FourCC("BC5U") ||
                     fourCC == FourCC("BC5S")) unit = 16;
            else if ((pfFlags & 0x40) != 0 && fourCC == 0 && bitCount % 8 == 0 && bitCount > 0) { unit = (int)bitCount / 8; blocks = false; }
            else return $"pixel format {FourCCName(fourCC)}";

            uint full = 1;
            for (uint s = Math.Max(width, height); s > 1; s >>= 1) full++;
            if (mips >= full) return "its mip chain is already complete";

            long Units(uint level)
            {
                uint w = Math.Max(1u, width >> (int)level), h = Math.Max(1u, height >> (int)level);
                return blocks ? (long)((w + 3) / 4) * ((h + 3) / 4) : (long)w * h;
            }
            long present = 0;
            for (uint l = 0; l < mips; l++) present += Units(l) * unit;
            if (dataStart + present > f.Length) return "its data is shorter than its header says";

            using (var o = new MemoryStream())
            {
                o.Write(f, 0, dataStart + (int)present);
                var last = new byte[unit];
                Array.Copy(f, dataStart + present - unit, last, 0, unit);
                for (uint l = mips; l < full; l++)
                    for (long u = 0; u < Units(l); u++) o.Write(last, 0, unit);
                byte[] r = o.ToArray();
                Put(r, 8, flags | FlagMipMapCount);
                Put(r, 28, full);
                Put(r, 108, caps | CapsComplex | CapsMipMap);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.WriteAllBytes(dst, r);
            }
            return null;
        }

        static uint U32(byte[] b, int at) => BitConverter.ToUInt32(b, at);
        static void Put(byte[] b, int at, uint v) => Array.Copy(BitConverter.GetBytes(v), 0, b, at, 4);
        static uint FourCC(string s) => (uint)(s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24);
        static string FourCCName(uint v) => v == 0 ? "uncompressed" : new string(new[] { (char)(v & 255), (char)(v >> 8 & 255), (char)(v >> 16 & 255), (char)(v >> 24) });
    }
}
