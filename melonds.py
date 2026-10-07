"""melonDS savestates: the memories in them, and a comparison with a run of the runtime.

    python -m ndskit.melonds STATE.ml1 --info
    python -m ndskit.melonds STATE.ml1 --peek ADDR[:LEN] ...
    python -m ndskit.melonds STATE.ml1 --compare RUNDIR [--ranges N] [--ignore LO-HI,...]

A savestate (melonDS 1.x, "MELN") is a list of sections, each a 4-byte
name, a 32-bit length (header included) and 8 more bytes, then its data.
Checked on a 2025 build against a cartridge's known memory:

    NDSG + 0x14              main RAM (4 MiB used; the section holds 16 MiB for the DSi)
    NDSG + 0x14 + 0x1000000  the shared WRAM (32 KiB)
         + 0x8000            the ARM7's WRAM (64 KiB)
    CP15 + 0x1C              the ITCM (32 KiB), then the DTCM (16 KiB)

--compare reads what ndskit's runtime writes when a run stops (ndsrun:
main.bin, itcm.bin, dtcm.bin, swram.bin, wram7.bin; cpus.txt with the DTCM's
base) and lists, for each memory, how many bytes differ and where: runs of
differing bytes merged across gaps of under 16, the largest first. The
VBlank count the SDK keeps (0x027FFC3C) is shown for both, to choose the
state and the run's length so the two stand at the same frame.
"""
import argparse
import os
import struct


class State:
    """A melonDS savestate's sections and its memories."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"MELN":
            raise ValueError("%s: not a melonDS savestate" % path)
        self.sections = {}
        i = 0x10
        while i + 16 <= len(self.data):
            name = self.data[i:i + 4].decode("latin1")
            length = struct.unpack_from("<I", self.data, i + 4)[0]
            if length < 16:
                break
            self.sections[name] = (i, length)
            i += length
        g = self.sections["NDSG"][0] + 0x14
        c = self.sections["CP15"][0] + 0x1C
        self.memories = {
            "main": self.data[g:g + 0x400000],
            "swram": self.data[g + 0x1000000:g + 0x1008000],
            "wram7": self.data[g + 0x1008000:g + 0x1018000],
            "itcm": self.data[c:c + 0x8000],
            "dtcm": self.data[c + 0x8000:c + 0xC000],
        }

    def read(self, addr, n, dtcm_base=0x027E0000):
        """n bytes at an ARM9 address (main RAM, the TCMs)."""
        m = self.memories
        if dtcm_base <= addr < dtcm_base + 0x4000:
            o = addr - dtcm_base
            return m["dtcm"][o:o + n]
        if 0x02000000 <= addr < 0x03000000:
            o = addr & 0x3FFFFF
            return m["main"][o:o + n]
        if addr < 0x02000000:
            o = addr & 0x7FFF
            return m["itcm"][o:o + n]
        raise ValueError("%08X: not in main RAM, ITCM or DTCM" % addr)

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]


# where each memory sits, for the comparison's addresses
BASES = {"main": 0x02000000, "itcm": 0x01FF8000, "dtcm": None, "swram": 0x037F8000, "wram7": 0x03800000}


def diff_ranges(a, b, gap=16):
    """[(start, end)] of the differing bytes, runs merged across gaps < gap."""
    out = []
    n = min(len(a), len(b))
    i = 0
    while i < n:
        if a[i] != b[i]:
            j = i + 1
            last = i
            while j < n and j - last < gap:
                if a[j] != b[j]:
                    last = j
                j += 1
            out.append((i, last + 1))
            i = last + 1
        else:
            i += 1
    return out


def run_info(rundir):
    info = {}
    p = os.path.join(rundir, "cpus.txt")
    if os.path.exists(p):
        for line in open(p):
            k, _, v = line.partition(" ")
            info[k] = v.strip()
    return info


def compare(state, rundir, nranges=12, ignore=()):
    info = run_info(rundir)
    dtcm = int(info.get("dtcm", "027E0000"), 16)
    run = {}
    for name in ("main", "itcm", "dtcm", "swram", "wram7"):
        p = os.path.join(rundir, name + ".bin")
        if os.path.exists(p):
            with open(p, "rb") as f:
                run[name] = f.read()
    vb_state = struct.unpack_from("<I", state.memories["main"], 0x3FFC3C)[0]
    vb_run = struct.unpack_from("<I", run["main"], 0x3FFC3C)[0] if "main" in run else None
    print("SDK VBlank count (0x027FFC3C): state %d, run %s (the runtime's own: %s)"
          % (vb_state, vb_run, info.get("vblanks", "?")))
    for name, mine in run.items():
        theirs = state.memories[name]
        base = dtcm if name == "dtcm" else BASES[name]
        ranges = [(s, e) for s, e in diff_ranges(theirs, mine)
                  if not any(lo <= base + s and base + e <= hi for lo, hi in ignore)]
        nbytes = sum(sum(1 for i in range(s, e) if theirs[i] != mine[i]) for s, e in ranges)
        print("%-6s %8d bytes differ in %d ranges" % (name, nbytes, len(ranges)))
        for s, e in sorted(ranges, key=lambda r: r[0] - r[1])[:nranges]:
            k = min(e - s, 16)
            print("    %08X-%08X (%5d)  melonDS %s  run %s" % (
                base + s, base + e, e - s, theirs[s:s + k].hex(), mine[s:s + k].hex()))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("state")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--peek", nargs="*", default=[], help="ADDR[:LEN], hex")
    ap.add_argument("--compare", metavar="RUNDIR")
    ap.add_argument("--ranges", type=int, default=12)
    ap.add_argument("--ignore", default="", help="LO-HI,... (hex): ranges left out of the comparison")
    a = ap.parse_args(argv)
    st = State(a.state)
    if a.info:
        for name, (o, n) in sorted(st.sections.items(), key=lambda x: x[1][0]):
            print("%-4s at %08X, %08X bytes" % (name, o, n))
        print("SDK VBlank count: %d" % st.u32(0x027FFC3C))
    for p in a.peek:
        addr, _, n = p.partition(":")
        addr, n = int(addr, 16), int(n or "10", 16)
        print("%08X  %s" % (addr, st.read(addr, n).hex()))
    if a.compare:
        ignore = []
        for r in a.ignore.split(","):
            if r:
                lo, _, hi = r.partition("-")
                ignore.append((int(lo, 16), int(hi, 16)))
        compare(st, a.compare, a.ranges, ignore)


if __name__ == "__main__":
    main()
