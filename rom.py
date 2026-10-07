"""DS cartridge images (.nds/.srl): header, banner, code, file system.

    python -m ndskit.rom GAME.nds --info            # header, CRCs, code, autoloads, SDK
    python -m ndskit.rom GAME.nds --list            # the file system, with each file's kind
    python -m ndskit.rom GAME.nds --extract DIR     # files + sys/ (header, arm9, arm7, overlays, banner)
    python -m ndskit.rom GAME.nds --extract DIR --nested   # and NitroROM archives inside it

A DS cartridge is one address space read by offset: a 0x4000-byte header
(the first 0x200 bytes meaningful), then the two CPUs' programs, the
file name table (FNT) and file allocation table (FAT), overlays, the
banner (icon and titles). The FNT is a tree of directories whose entries
name files by consecutive ids; the FAT gives each id its [start, end) in
the image. Overlays are files too: the overlay tables (OVT) give each one
its load address, sizes and static initialisers.

The ARM9 program built with Nintendo's NitroSDK ends with a footer
(0xDEC00621, the offset of _start_ModuleParams, 0) after its last byte.
ModuleParams gives the autoload list (blocks the crt0 copies to ITCM,
DTCM and elsewhere at boot), the static BSS, the end of the part
compressed with the SDK's backward LZ (0 when the program is stored
plain), and the SDK version. The first 0x800 bytes of the ARM9 are the
secure area's header: in a dump made by a decrypting reader they hold
0xE7FFDEFF (an undefined instruction) where the encrypted "encryObj"
marker was.

Archives: a game may keep NitroROM images inside its own file system
(the SDK's FS can mount one as an archive) -- whole .srl files with a
header, FNT and FAT of their own. `--nested` extracts their files beside
them (FILE.srl.d/).
"""
import argparse
import os
import struct
import sys
import zlib

LANGS = ["Japanese", "English", "French", "German", "Italian", "Spanish", "Chinese", "Korean"]

MAGICS = [  # (bytes at 0, kind) for --list; Nintendo's container formats and a few common ones
    (b"RGCN", "NCGR (2D characters)"), (b"RLCN", "NCLR (palette)"), (b"RCSN", "NSCR (2D screen)"),
    (b"RECN", "NCER (2D cells)"), (b"RNAN", "NANR (cell animation)"), (b"RNMC", "NMCR (multi-cell)"),
    (b"RNMA", "NMAR (multi-cell animation)"), (b"RTFN", "NFTR (font)"), (b"NARC", "NARC (archive)"),
    (b"SDAT", "SDAT (sound archive)"), (b"SSEQ", "SSEQ (sequence)"), (b"SBNK", "SBNK (bank)"),
    (b"SWAR", "SWAR (wave archive)"), (b"STRM", "STRM (stream)"), (b"BMD0", "NSBMD (3D model)"),
    (b"BTX0", "NSBTX (3D textures)"), (b"BCA0", "NSBCA (3D animation)"), (b"BTP0", "NSBTP"),
    (b"BTA0", "NSBTA"), (b"BMA0", "NSBMA"), (b"BVA0", "NSBVA"), (b"MODS", "Mobiclip video (MODS)"),
    (b"MESG", "BMG (messages)"), (b"NINTENDO    ", "NitroROM image"),
]


def crc16(data, crc=0xFFFF):
    """The CRC-16 of the DS header, banner and secure area (reflected 0x8005, init 0xFFFF)."""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def blz_decompress(data):
    """Undo the SDK's backward LZ ("BLZ"), applied to the end of an ARM9 program or overlay.

    The last 8 bytes: a word whose low 24 bits are the length of the compressed
    part (counted back from the end) and whose top byte is the footer's length,
    then a word with how many bytes decompression adds. The compressed part is
    read and written from its end backwards; the bytes before it are stored plain.
    """
    if len(data) < 8:
        return data
    end, extra = struct.unpack_from("<II", data, len(data) - 8)
    if extra == 0:
        return data
    clen, hlen = end & 0xFFFFFF, end >> 24
    out = bytearray(data) + bytearray(extra)
    src = len(data) - hlen
    dst = len(out)
    stop = len(data) - clen
    while src > stop and dst > stop:
        src -= 1
        flags = data[src]
        for _ in range(8):
            if src <= stop or dst <= stop:
                break
            if flags & 0x80:
                src -= 2
                pair = data[src + 1] << 8 | data[src]     # read backwards: the high byte first
                n, disp = (pair >> 12) + 3, (pair & 0xFFF) + 3
                for _ in range(min(n, dst - stop)):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = data[src]
            flags <<= 1
    return bytes(out)


class Code:
    """One CPU's program: where it is in the image, where it loads, where it starts."""

    def __init__(self, rom, base_off):
        self.offset, self.entry, self.ram, self.size = struct.unpack_from("<4I", rom.data, base_off)
        self.data = rom.data[self.offset:self.offset + self.size]
        self.plain = self.data      # decompressed, when ModuleParams says it is compressed


class ModuleParams:
    """NitroSDK's _start_ModuleParams of an ARM9 program (see the module docstring)."""

    def __init__(self, code, data, offset):
        f = struct.unpack_from("<7I", data, offset)
        self.address = code.ram + offset
        (self.autoload_list, self.autoload_list_end, self.autoload_start,
         self.bss_start, self.bss_end, self.compressed_end, self.sdk_version) = f
        self.autoloads = []
        src = self.autoload_start
        for p in range(self.autoload_list, self.autoload_list_end, 12):
            addr, size, bss = struct.unpack_from("<3I", data, p - code.ram)
            self.autoloads.append((addr, size, bss, src))
            src += size

    @property
    def sdk(self):
        v = self.sdk_version
        return "%d.%d (relstep %d)" % (v >> 24, (v >> 16) & 0xFF, v & 0xFFFF)


def module_params(rom, code):
    """Find ModuleParams through the footer after the ARM9 program, or None.

    ModuleParams itself lies in the plain part of a compressed program; the
    autoload list it points to may not, so the list is read from the
    program decompressed (code.plain).
    """
    foot = rom.data[code.offset + code.size:code.offset + code.size + 12]
    if len(foot) < 12 or struct.unpack_from("<I", foot)[0] != 0xDEC00621:
        return None
    off = struct.unpack_from("<I", foot, 4)[0]
    if off + 0x24 > len(code.data):
        return None
    comp_end = struct.unpack_from("<I", code.data, off + 0x14)[0]
    code.plain = code.data
    if comp_end:
        cut = comp_end - code.ram
        code.plain = blz_decompress(code.data[:cut]) + code.data[cut:]
    return ModuleParams(code, code.plain, off)


def module_params7(code):
    """The ARM7's ModuleParams, which has no footer: found by its shape.

    The SDK's ARM7 programs carry the same block as the ARM9's, near their
    start, and in both the autoload list is the last thing in the program:
    a block whose list ends exactly at the program's end, starts inside it
    and is a whole number of 12-byte entries is taken to be it.
    """
    end = code.ram + code.size
    for off in range(0, min(len(code.data), 0x2000) - 0x1C, 4):
        lst, lend, start = struct.unpack_from("<3I", code.data, off)
        if lend == end and code.ram <= lst < lend and (lend - lst) % 12 == 0 \
                and code.ram <= start < end and lend - lst <= 12 * 16:
            return ModuleParams(code, code.data, off)
    return None


class Overlay:
    def __init__(self, data, off):
        (self.id, self.ram, self.ram_size, self.bss_size, self.sinit_start, self.sinit_end,
         self.file_id, flags) = struct.unpack_from("<8I", data, off)
        self.compressed_size = flags & 0xFFFFFF
        self.compressed = bool(flags & 0x01000000)
        self.verify = bool(flags & 0x02000000)


class Rom:
    def __init__(self, data):
        self.data = data
        h = data
        self.title = h[0:12].rstrip(b"\0").decode("latin1")
        self.code = h[12:16].decode("latin1")
        self.maker = h[16:18].decode("latin1")
        self.unit = h[0x12]
        self.capacity = 128 << 10 << h[0x14]
        self.region = h[0x1D]
        self.version = h[0x1E]
        self.arm9 = Code(self, 0x20)
        self.arm7 = Code(self, 0x30)
        self.fnt_off, self.fnt_size, self.fat_off, self.fat_size = struct.unpack_from("<4I", h, 0x40)
        o9, s9, o7, s7 = struct.unpack_from("<4I", h, 0x50)
        self.banner_off = struct.unpack_from("<I", h, 0x68)[0]
        self.used_size = struct.unpack_from("<I", h, 0x80)[0]
        self.header_size = struct.unpack_from("<I", h, 0x84)[0]
        self.header_crc = struct.unpack_from("<H", h, 0x15E)[0]
        self.logo_crc = struct.unpack_from("<H", h, 0x15C)[0]
        self.secure_crc = struct.unpack_from("<H", h, 0x6C)[0]
        self.fat = [struct.unpack_from("<II", data, self.fat_off + 8 * i) for i in range(self.fat_size // 8)]
        self.overlays9 = [Overlay(data, o9 + i) for i in range(0, s9, 32)]
        self.overlays7 = [Overlay(data, o7 + i) for i in range(0, s7, 32)]
        self.params = module_params(self, self.arm9)
        self.params7 = module_params7(self.arm7)
        self.names = {}     # file id -> path
        self.dirs = {}      # dir id -> path
        if self.fnt_size:
            self._walk(0xF000, "")

    def _walk(self, did, path):
        self.dirs[did] = path
        eoff, fid, _ = struct.unpack_from("<IHH", self.data, self.fnt_off + 8 * (did & 0xFFF))
        p = self.fnt_off + eoff
        while True:
            b = self.data[p]
            p += 1
            if b == 0:
                break
            n = self.data[p:p + (b & 0x7F)].decode("latin1")
            p += b & 0x7F
            if b & 0x80:
                sub = struct.unpack_from("<H", self.data, p)[0]
                p += 2
                self._walk(sub, path + n + "/")
            else:
                self.names[fid] = path + n
                fid += 1

    def file(self, fid):
        s, e = self.fat[fid]
        return self.data[s:e]

    def find(self, path):
        for fid, n in self.names.items():
            if n == path:
                return fid
        return None

    @property
    def secure_area(self):
        if self.arm9.offset != 0x4000:
            return "none (ARM9 not at 0x4000)"
        w = struct.unpack_from("<II", self.data, 0x4000)
        if w == (0xE7FFDEFF, 0xE7FFDEFF):
            return "decrypted (0xE7FFDEFF marker)"
        if self.data[0x4000:0x4800] == bytes(0x800):
            return "zeroed"
        return "encrypted or unknown (%08X %08X)" % w

    def banner(self):
        """(version, icon RGBA 32x32 bytes, {language: title}) or None."""
        b = self.banner_off
        if not b or b + 0x840 > len(self.data):
            return None
        ver = struct.unpack_from("<H", self.data, b)[0]
        pal = [struct.unpack_from("<H", self.data, b + 0x220 + 2 * i)[0] for i in range(16)]
        rgba = bytearray()
        for y in range(32):
            for x in range(32):
                tile = (y // 8) * 4 + x // 8
                byte = self.data[b + 0x20 + tile * 32 + (y % 8) * 4 + (x % 8) // 2]
                idx = byte >> 4 if x & 1 else byte & 15
                c = pal[idx]
                rgba += bytes(((c & 31) * 255 // 31, (c >> 5 & 31) * 255 // 31,
                               (c >> 10 & 31) * 255 // 31, 0 if idx == 0 else 255))
        nlang = 6 + (ver >= 2) + (ver >= 3)
        titles = {}
        for i in range(nlang):
            t = self.data[b + 0x240 + 0x100 * i:b + 0x340 + 0x100 * i].decode("utf-16-le").split("\0")[0]
            titles[LANGS[i]] = t
        return ver, bytes(rgba), titles


def kind(data):
    for m, k in MAGICS:
        if data.startswith(m):
            return k
    return ""


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def info(rom, out=print):
    out("title       %s   code %s   maker %s   unit %d   version %d   region %d"
        % (rom.title, rom.code, rom.maker, rom.unit, rom.version, rom.region))
    out("capacity    %d MiB (used 0x%X bytes)" % (rom.capacity >> 20, rom.used_size))
    out("header CRC  %04X (%s)   logo CRC %04X (%s)" % (
        rom.header_crc, "ok" if crc16(rom.data[:0x15E]) == rom.header_crc else "BAD",
        rom.logo_crc, "ok" if crc16(rom.data[0xC0:0x15C]) == rom.logo_crc else "BAD"))
    out("secure area %s, header CRC %04X" % (rom.secure_area, rom.secure_crc))
    for n, c in (("ARM9", rom.arm9), ("ARM7", rom.arm7)):
        out("%s        image 0x%08X size 0x%06X  ram 0x%08X  entry 0x%08X"
            % (n, c.offset, c.size, c.ram, c.entry))
    p = rom.params
    if p:
        out("ModuleParams at 0x%08X: SDK %s" % (p.address, p.sdk))
        out("  static BSS 0x%08X-0x%08X; compressed: %s" % (
            p.bss_start, p.bss_end, "no" if not p.compressed_end else "up to 0x%08X" % p.compressed_end))
        for addr, size, bss, src in p.autoloads:
            out("  autoload 0x%08X size 0x%X bss 0x%X  (from 0x%08X)  %s"
                % (addr, size, bss, src, region_name(addr)))
    p7 = rom.params7
    if p7:
        out("ARM7 ModuleParams at 0x%08X (found by its shape)" % p7.address)
        for addr, size, bss, src in p7.autoloads:
            out("  autoload 0x%08X size 0x%X bss 0x%X  (from 0x%08X)  %s"
                % (addr, size, bss, src, region_name(addr, 7)))
    out("FNT         0x%08X size 0x%X; FAT 0x%08X, %d files in %d directories"
        % (rom.fnt_off, rom.fnt_size, rom.fat_off, len(rom.fat), len(rom.dirs)))
    out("overlays    ARM9 %d, ARM7 %d" % (len(rom.overlays9), len(rom.overlays7)))
    for side, ovs in (("ARM9", rom.overlays9), ("ARM7", rom.overlays7)):
        for o in ovs:
            out("  %s overlay %d: file %d ram 0x%08X size 0x%X bss 0x%X sinit 0x%08X-0x%08X%s"
                % (side, o.id, o.file_id, o.ram, o.ram_size, o.bss_size, o.sinit_start, o.sinit_end,
                   " compressed (0x%X)" % o.compressed_size if o.compressed else ""))
    b = rom.banner()
    if b:
        ver, _, titles = b
        out("banner      version %d at 0x%08X" % (ver, rom.banner_off))
        for lang, t in titles.items():
            out("  %-9s %s" % (lang, t.replace("\n", " / ")))


def region_name(addr, cpu=9):
    try:
        from . import hw
        return hw.name(addr, cpu) or ""
    except ImportError:
        return ""


def extract(rom, outdir, nested=False, out=print):
    os.makedirs(os.path.join(outdir, "sys"), exist_ok=True)

    def put(rel, data):
        path = os.path.join(outdir, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    put("sys/header.bin", rom.data[:0x4000])
    put("sys/arm9.bin", rom.arm9.data)
    put("sys/arm7.bin", rom.arm7.data)
    if rom.arm9.plain is not rom.arm9.data:
        put("sys/arm9.dec.bin", rom.arm9.plain)
    b = rom.banner()
    if b:
        put("sys/banner.bin", rom.data[rom.banner_off:rom.banner_off + 0x840])
        write_png(os.path.join(outdir, "sys", "icon.png"), 32, 32, b[1])
    named = set(rom.names)
    for side, ovs in (("arm9", rom.overlays9), ("arm7", rom.overlays7)):
        for o in ovs:
            data = rom.file(o.file_id)
            put("sys/overlay_%s_%04d.bin" % (side, o.id), data)
            if o.compressed:
                put("sys/overlay_%s_%04d.dec.bin" % (side, o.id), blz_decompress(data))
            named.add(o.file_id)
    n = 0
    for fid, path in sorted(rom.names.items()):
        data = rom.file(fid)
        put(path, data)
        n += 1
        if nested and data.startswith(b"NINTENDO    ") or nested and path.lower().endswith(".srl"):
            sub = Rom(data)
            k = extract(sub, os.path.join(outdir, *path.split("/")) + ".d", nested, out=lambda *a: None)
            out("  %s: NitroROM image, %d files extracted to %s.d/" % (path, k, path))
    left = [i for i in range(len(rom.fat)) if i not in named and rom.fat[i][1] > rom.fat[i][0]]
    for i in left:
        put("sys/unnamed_%04d.bin" % i, rom.file(i))
    out("%d files, %d overlays, %d unnamed -> %s" % (n, len(rom.overlays9) + len(rom.overlays7), len(left), outdir))
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", metavar="DIR")
    ap.add_argument("--nested", action="store_true", help="also extract NitroROM images found inside")
    a = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    with open(a.image, "rb") as f:
        rom = Rom(f.read())
    if a.info or not (a.list or a.extract):
        info(rom)
    if a.list:
        for fid, path in sorted(rom.names.items()):
            s, e = rom.fat[fid]
            print("%5d  %08X  %9d  %-40s %s" % (fid, s, e - s, path, kind(rom.data[s:s + 12])))
    if a.extract:
        extract(rom, a.extract, a.nested)


if __name__ == "__main__":
    main()
