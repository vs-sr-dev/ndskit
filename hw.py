"""The DS's address maps and I/O register names, for annotating code.

    python -m ndskit.hw 04000000 04000400 027E0000        # as the ARM9 sees them
    python -m ndskit.hw --arm7 04000400 040001C0

name(addr, cpu=9) returns "DISPCNT", "GXFIFO", "main RAM"... or None.

The two CPUs see different maps. Both have the 4 MiB main RAM at
0x02000000 (mirrored up to 0x02FFFFFF), the shared WRAM at 0x03000000
(given to one CPU or split by WRAMCNT), the I/O registers at 0x04000000
and the IPC FIFO's receive port at 0x04100000. The ARM9 also has its two
tightly coupled memories, placed by CP15 (NitroSDK puts ITCM at 0 with
the mirror at 0x01FF8000 its programs link to, DTCM where the program
says, often 0x027E0000), the palettes, VRAM (the 2D engines' BG and OBJ
windows, and the LCDC window onto the banks that VRAMCNT maps) and OAM,
and its BIOS at 0xFFFF0000. The ARM7 has its own 64 KiB WRAM at
0x03800000, the sound registers where the ARM9 has the 3D engine
(0x04000400), the SPI bus (touch screen, firmware, power), the RTC, and
the wireless hardware at 0x04800000.

Register offsets follow GBATEK (Martin Korth's hardware reference).
"""
import argparse

REGIONS9 = [
    (0x00000000, 0x00008000, "ITCM"),
    (0x01FF8000, 0x02000000, "ITCM (SDK mirror)"),
    (0x02000000, 0x03000000, "main RAM"),
    (0x027E0000, 0x027E4000, "DTCM (SDK default)"),
    (0x03000000, 0x04000000, "shared WRAM"),
    (0x04000000, 0x04001100, "I/O"),
    (0x04100000, 0x04100014, "I/O (IPC/card ports)"),
    (0x05000000, 0x05000400, "palette A"),
    (0x05000400, 0x05000800, "palette B"),
    (0x06000000, 0x06200000, "VRAM BG A"),
    (0x06200000, 0x06400000, "VRAM BG B"),
    (0x06400000, 0x06600000, "VRAM OBJ A"),
    (0x06600000, 0x06800000, "VRAM OBJ B"),
    (0x06800000, 0x068A4000, "VRAM LCDC"),
    (0x07000000, 0x07000400, "OAM A"),
    (0x07000400, 0x07000800, "OAM B"),
    (0x08000000, 0x0A010000, "GBA slot"),
    (0xFFFF0000, 0xFFFF8000, "BIOS"),
]

REGIONS7 = [
    (0x00000000, 0x00004000, "BIOS"),
    (0x02000000, 0x03000000, "main RAM"),
    (0x03000000, 0x03800000, "shared WRAM"),
    (0x03800000, 0x04000000, "ARM7 WRAM"),
    (0x04000000, 0x04000520, "I/O"),
    (0x04100000, 0x04100014, "I/O (IPC/card ports)"),
    (0x04800000, 0x04810000, "wireless"),
    (0x06000000, 0x07000000, "VRAM (banks C/D as ARM7 RAM)"),
    (0x08000000, 0x0A010000, "GBA slot"),
]

# 2D engine registers; engine B repeats engine A's at +0x1000 (no 3D, capture, FIFO there)
_2D = {
    0x00: "DISPCNT", 0x08: "BG0CNT", 0x0A: "BG1CNT", 0x0C: "BG2CNT", 0x0E: "BG3CNT",
    0x10: "BG0HOFS", 0x12: "BG0VOFS", 0x14: "BG1HOFS", 0x16: "BG1VOFS",
    0x18: "BG2HOFS", 0x1A: "BG2VOFS", 0x1C: "BG3HOFS", 0x1E: "BG3VOFS",
    0x20: "BG2PA", 0x22: "BG2PB", 0x24: "BG2PC", 0x26: "BG2PD", 0x28: "BG2X", 0x2C: "BG2Y",
    0x30: "BG3PA", 0x32: "BG3PB", 0x34: "BG3PC", 0x36: "BG3PD", 0x38: "BG3X", 0x3C: "BG3Y",
    0x40: "WIN0H", 0x42: "WIN1H", 0x44: "WIN0V", 0x46: "WIN1V", 0x48: "WININ", 0x4A: "WINOUT",
    0x4C: "MOSAIC", 0x50: "BLDCNT", 0x52: "BLDALPHA", 0x54: "BLDY", 0x6C: "MASTER_BRIGHT",
}

COMMON = {  # registers at the same place on both CPUs
    0x004: "DISPSTAT", 0x006: "VCOUNT",
    0x0B0: "DMA0SAD", 0x0B4: "DMA0DAD", 0x0B8: "DMA0CNT", 0x0BC: "DMA1SAD", 0x0C0: "DMA1DAD",
    0x0C4: "DMA1CNT", 0x0C8: "DMA2SAD", 0x0CC: "DMA2DAD", 0x0D0: "DMA2CNT", 0x0D4: "DMA3SAD",
    0x0D8: "DMA3DAD", 0x0DC: "DMA3CNT",
    0x100: "TM0CNT_L", 0x102: "TM0CNT_H", 0x104: "TM1CNT_L", 0x106: "TM1CNT_H",
    0x108: "TM2CNT_L", 0x10A: "TM2CNT_H", 0x10C: "TM3CNT_L", 0x10E: "TM3CNT_H",
    0x130: "KEYINPUT", 0x132: "KEYCNT",
    0x180: "IPCSYNC", 0x184: "IPCFIFOCNT", 0x188: "IPCFIFOSEND",
    0x1A0: "AUXSPICNT", 0x1A2: "AUXSPIDATA", 0x1A4: "ROMCTRL", 0x1A8: "CARD_COMMAND",
    0x1B0: "CARD_SEED0_L", 0x1B4: "CARD_SEED1_L", 0x1B8: "CARD_SEED0_H", 0x1BA: "CARD_SEED1_H",
    0x204: "EXMEMCNT", 0x208: "IME", 0x210: "IE", 0x214: "IF",
}

ARM9 = dict(COMMON)
ARM9.update({k: v for k, v in _2D.items()})
ARM9.update({0x1000 + k: v + "_B" for k, v in _2D.items()})
ARM9.update({
    0x060: "DISP3DCNT", 0x064: "DISPCAPCNT", 0x068: "DISP_MMEM_FIFO",
    0x0E0: "DMA0FILL", 0x0E4: "DMA1FILL", 0x0E8: "DMA2FILL", 0x0EC: "DMA3FILL",
    0x240: "VRAMCNT_A", 0x241: "VRAMCNT_B", 0x242: "VRAMCNT_C", 0x243: "VRAMCNT_D",
    0x244: "VRAMSTAT/VRAMCNT_E", 0x245: "VRAMCNT_F", 0x246: "VRAMCNT_G", 0x247: "WRAMCNT",
    0x248: "VRAMCNT_H", 0x249: "VRAMCNT_I",
    0x280: "DIVCNT", 0x290: "DIV_NUMER", 0x298: "DIV_DENOM", 0x2A0: "DIV_RESULT", 0x2A8: "DIVREM_RESULT",
    0x2B0: "SQRTCNT", 0x2B4: "SQRT_RESULT", 0x2B8: "SQRT_PARAM",
    0x300: "POSTFLG", 0x304: "POWCNT1",
    # 3D engine: rendering control
    0x320: "RDLINES_COUNT", 0x330: "EDGE_COLOR", 0x340: "ALPHA_TEST_REF", 0x350: "CLEAR_COLOR",
    0x354: "CLEAR_DEPTH", 0x356: "CLRIMAGE_OFFSET", 0x358: "FOG_COLOR", 0x35C: "FOG_OFFSET",
    0x360: "FOG_TABLE", 0x380: "TOON_TABLE",
    # geometry engine: the FIFO and the command ports
    0x400: "GXFIFO",
    0x440: "MTX_MODE", 0x444: "MTX_PUSH", 0x448: "MTX_POP", 0x44C: "MTX_STORE", 0x450: "MTX_RESTORE",
    0x454: "MTX_IDENTITY", 0x458: "MTX_LOAD_4x4", 0x45C: "MTX_LOAD_4x3", 0x460: "MTX_MULT_4x4",
    0x464: "MTX_MULT_4x3", 0x468: "MTX_MULT_3x3", 0x46C: "MTX_SCALE", 0x470: "MTX_TRANS",
    0x480: "COLOR", 0x484: "NORMAL", 0x488: "TEXCOORD", 0x48C: "VTX_16", 0x490: "VTX_10",
    0x494: "VTX_XY", 0x498: "VTX_XZ", 0x49C: "VTX_YZ", 0x4A0: "VTX_DIFF", 0x4A4: "POLYGON_ATTR",
    0x4A8: "TEXIMAGE_PARAM", 0x4AC: "PLTT_BASE", 0x4C0: "DIF_AMB", 0x4C4: "SPE_EMI",
    0x4C8: "LIGHT_VECTOR", 0x4CC: "LIGHT_COLOR", 0x4D0: "SHININESS", 0x500: "BEGIN_VTXS",
    0x504: "END_VTXS", 0x540: "SWAP_BUFFERS", 0x580: "VIEWPORT", 0x5C0: "BOX_TEST",
    0x5C4: "POS_TEST", 0x5C8: "VEC_TEST",
    0x600: "GXSTAT", 0x604: "RAM_COUNT", 0x610: "DISP_1DOT_DEPTH", 0x620: "POS_RESULT",
    0x630: "VEC_RESULT", 0x640: "CLIPMTX_RESULT", 0x680: "VECMTX_RESULT",
})

ARM7 = dict(COMMON)
ARM7.update({
    0x134: "RCNT", 0x136: "EXTKEYIN", 0x138: "RTC",
    0x1C0: "SPICNT", 0x1C2: "SPIDATA",
    0x240: "VRAMSTAT", 0x241: "WRAMSTAT", 0x300: "POSTFLG", 0x301: "HALTCNT", 0x304: "POWCNT2",
    0x308: "BIOSPROT",
    0x500: "SOUNDCNT", 0x504: "SOUNDBIAS", 0x508: "SNDCAP0CNT", 0x509: "SNDCAP1CNT",
    0x510: "SNDCAP0DAD", 0x514: "SNDCAP0LEN", 0x518: "SNDCAP1DAD", 0x51C: "SNDCAP1LEN",
})
for _ch in range(16):  # the 16 sound channels at 0x04000400 + 16*n
    for _o, _n in ((0, "CNT"), (4, "SAD"), (8, "TMR"), (10, "PNT"), (12, "LEN")):
        ARM7[0x400 + 16 * _ch + _o] = "SOUND%d%s" % (_ch, _n)

PORTS = {0x04100000: "IPCFIFORECV", 0x04100010: "CARD_DATA_RD"}


def name(addr, cpu=9):
    """The register at addr, or the region it falls in, or None."""
    addr &= 0xFFFFFFFF
    if addr in PORTS:
        return PORTS[addr]
    if 0x04000000 <= addr < 0x04002000:
        regs = ARM9 if cpu == 9 else ARM7
        off = addr - 0x04000000
        if off in regs:
            return regs[off]
        for back in (1, 2, 3):              # inside a wider register
            if off - back in regs:
                return "%s+%d" % (regs[off - back], back)
    best = None
    for lo, hi, n in (REGIONS9 if cpu == 9 else REGIONS7):
        if lo <= addr < hi and (best is None or hi - lo < best[1] - best[0]):
            best = (lo, hi, n)
    return best[2] if best else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("addr", nargs="+")
    ap.add_argument("--arm7", action="store_true", help="the ARM7's map")
    a = ap.parse_args(argv)
    for s in a.addr:
        v = int(s, 16)
        print("%08X  %s" % (v, name(v, 7 if a.arm7 else 9) or "?"))


if __name__ == "__main__":
    main()
