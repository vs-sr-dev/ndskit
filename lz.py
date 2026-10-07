"""The compressions of the GBA/DS BIOS (and the SDK's MI library): LZ77, its LZ11 variant, RLE.

    python -m ndskit.lz FILE [--out OUT]          # decompress one file
    python -m ndskit.lz --scan DIR                # which files are compressed, and how

Every format starts with a word: the type in its low byte (0x10 LZ77,
0x11 LZ11, 0x30 RLE; 0x2x Huffman, not done), the decompressed size in
the top 24 bits (LZ11 and others: a further word when those are 0).

LZ77 (the BIOS's LZ77UnComp, SWI 0x11/0x12): a flag byte, read from its
top bit down, for each of the next eight items: 0 a literal byte, 1 a
copy of 3-18 bytes from 1-4096 bytes back (two bytes: length-3 in the
top nibble, distance-1 in the other 12 bits). LZ11 (not in the BIOS, used
by later SDK tools) lets the top nibble select longer lengths. RLE: a
flag byte, its top bit 1 for 3-130 copies of the next byte, 0 for 1-128
literal bytes.
"""
import argparse
import os
import struct


def header(data):
    """(type, size, offset of the data) or None if the first word cannot be one of ours."""
    if len(data) < 4:
        return None
    w = struct.unpack_from("<I", data)[0]
    kind, size, off = w & 0xFF, w >> 8, 4
    if kind not in (0x10, 0x11, 0x30):
        return None
    if size == 0 and len(data) >= 8:
        size, off = struct.unpack_from("<I", data, 4)[0], 8
    return kind, size, off


def lz10(data, size, p=4):
    out = bytearray()
    while len(out) < size:
        flags = data[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0, b1 = data[p], data[p + 1]
                p += 2
                n, disp = (b0 >> 4) + 3, ((b0 & 0xF) << 8 | b1) + 1
                if disp > len(out):
                    raise ValueError("copy from before the start at %d" % p)
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(data[p])
                p += 1
    return bytes(out[:size]), p


def lz11(data, size, p=4):
    out = bytearray()
    while len(out) < size:
        flags = data[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0 = data[p]
                top = b0 >> 4
                if top == 0:            # 8-bit length + 17
                    n = ((b0 & 0xF) << 4 | data[p + 1] >> 4) + 0x11
                    disp = ((data[p + 1] & 0xF) << 8 | data[p + 2]) + 1
                    p += 3
                elif top == 1:          # 16-bit length + 273
                    n = ((b0 & 0xF) << 12 | data[p + 1] << 4 | data[p + 2] >> 4) + 0x111
                    disp = ((data[p + 2] & 0xF) << 8 | data[p + 3]) + 1
                    p += 4
                else:
                    n = top + 1
                    disp = ((b0 & 0xF) << 8 | data[p + 1]) + 1
                    p += 2
                if disp > len(out):
                    raise ValueError("copy from before the start at %d" % p)
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(data[p])
                p += 1
    return bytes(out[:size]), p


def rle(data, size, p=4):
    out = bytearray()
    while len(out) < size:
        f = data[p]
        p += 1
        if f & 0x80:
            out += bytes([data[p]]) * ((f & 0x7F) + 3)
            p += 1
        else:
            n = (f & 0x7F) + 1
            out += data[p:p + n]
            p += n
    return bytes(out[:size]), p


def decompress(data):
    """The decompressed bytes; ValueError if data is not one of these formats."""
    h = header(data)
    if not h:
        raise ValueError("not a BIOS-compressed file")
    kind, size, off = h
    fn = {0x10: lz10, 0x11: lz11, 0x30: rle}[kind]
    try:
        out, end = fn(data, size, off)
    except IndexError:
        raise ValueError("ran out of input")
    return out


def probe(data):
    """'lz10'/'lz11'/'rle' with the sizes, if the whole stream decodes and ends within the file."""
    h = header(data)
    if not h:
        return None
    kind, size, off = h
    if size < 4 or size > 64 * len(data) + 64:
        return None
    try:
        out, end = {0x10: lz10, 0x11: lz11, 0x30: rle}[kind](data, size, off)
    except (IndexError, ValueError):
        return None
    if end > len(data) or len(data) - end > 16:     # trailing padding to 4 bytes is normal
        return None
    return {0x10: "lz10", 0x11: "lz11", 0x30: "rle"}[kind], size


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("path")
    ap.add_argument("--out")
    ap.add_argument("--scan", action="store_true", help="path is a directory: report every compressed file")
    a = ap.parse_args(argv)
    if a.scan:
        counts = {}
        for dp, _, fs in os.walk(a.path):
            for fn in sorted(fs):
                with open(os.path.join(dp, fn), "rb") as f:
                    data = f.read()
                r = probe(data)
                ext = os.path.splitext(fn)[1].lower()
                key = (ext, r[0] if r else "-")
                counts[key] = counts.get(key, 0) + 1
        for (ext, k), n in sorted(counts.items()):
            print("%-8s %-5s %5d" % (ext, k, n))
        return
    with open(a.path, "rb") as f:
        data = f.read()
    out = decompress(data)
    print("%s: %d -> %d bytes" % (a.path, len(data), len(out)))
    if a.out:
        with open(a.out, "wb") as f:
            f.write(out)


if __name__ == "__main__":
    main()
