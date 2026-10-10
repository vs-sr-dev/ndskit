"""Recompile a DS cartridge's programs (or flat ARM images) to C++, one module per program.

    python -m ndskit.recomp GAME.nds --out build/recomp [--cpu 9,7] [--per-file 8000]
                            [--no-comments] [--optest] [--copy CPU:DEST=SRC:LEN ...]
    python -m ndskit.recomp --image NAME=FILE@BASE:CPU[+SEED,...] ... --out DIR

Each program is discovered (recomp/discover.py) where it runs, in segments
(the main block and each autoload at its destination), and emitted into its
own namespace, p_<name> (p_arm9, p_arm7). The runtime activates, for each
CPU, the module whose segments it finds in memory (arm_identify, by each
segment's crc32). --copy adds a block the program copies elsewhere at run
time and runs there (code moved to the ITCM for speed): the bytes at SRC,
LEN long, as a segment at DEST, discovered from DEST; the runtime names
such a block when it meets a call into one.

A program with overlays (unless --no-overlays) gets one more module per
overlay, p_<name>_ov<id>: each overlay is discovered with the program, loaded
at its address; the overlay's module holds its own functions and calls the
program's directly, and the program's module holds every function of its
own that some overlay's run found. Calls into an overlay's load area go
through the runtime, which looks them up among the overlays' modules: the
function whose bytes are in memory as built runs (the module lists each
function's extent, its code and literal pools, with its crc32); a function
the emitter cannot take (bytes that are no code as they lie: encrypted) and
code no module holds are left to the runtime's interpreter. Writes, into --out:

    p_<name>_funcs.h        prototypes and the module descriptor
    p_<name>_NNN.cpp        the functions, split by instruction count
    p_<name>_table.cpp      the module: segments with their crc32, entries,
                            the landings (returns to a context save's callers)
    modules.cpp             every module of the build (g_arm_modules)
    CMakeLists.txt          the library `recomp`, the runtime, the self-test,
                            ndsrun (the machine, headless)
    report.txt              per module: functions, the transfers through
                            registers by how they were resolved, targets
                            that are not entries

--optest adds ndskit's own instruction test (recomp/selftest.py): synthetic
programs with every instruction form, for each CPU, as modules OPTEST9 and
OPTEST7, and their vectors in --out/selftest/.
"""
import argparse
import os
import time
import zlib

from . import discover
from . import emit as E

RUNTIME = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "runtime"))


class Out:
    """A generated file, written only if its content changed: a rebuild after
    a small change recompiles only what it touched."""

    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, s):
        self.parts.append(s)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        text = "".join(self.parts)
        try:
            with open(self.path, encoding="utf-8", newline="") as f:
                if f.read() == text:
                    return
        except OSError:
            pass
        with open(self.path, "w", encoding="utf-8", newline="") as f:
            f.write(text)


def parse_image(spec):
    """NAME=FILE@BASE:CPU[+SEED,...] -> (name, path, base, cpu, seeds); odd seeds are Thumb."""
    name, _, rest = spec.partition("=")
    path, _, where = rest.rpartition("@")
    where, _, seeds = where.partition("+")
    base, _, cpu = where.partition(":")
    return name, path, int(base, 16), int(cpu or 9), [int(s, 16) for s in seeds.split(",") if s]


def parse_copy(spec):
    """CPU:DEST=SRC:LEN (hex) -> (cpu, dest, src, len)."""
    cpu, _, rest = spec.partition(":")
    dest, _, rest = rest.partition("=")
    src, _, n = rest.partition(":")
    return int(cpu), int(dest, 16), int(src, 16), int(n, 16)


def copy_segment(segs, dest, src, n):
    """A block of a program that it copies elsewhere at run time (code made
    to run from the ITCM, say), as a segment at its destination."""
    for s in segs:
        if s.base <= src and src + n <= s.end:
            return discover.Segment(dest, s.data[src - s.base:src - s.base + n], "copy")
    raise SystemExit("--copy: %08X-%08X is not inside one segment of the program" % (src, src + n))


def loaded_segment(rom, cpu):
    """A program stored compressed (it decompresses itself in its crt0), as the
    cartridge loads it: what memory holds at the boot, where the runtime looks
    for the program's module. None when the program is stored plain."""
    code = rom.arm9 if cpu == 9 else rom.arm7
    if code.data == code.plain:
        return None
    return discover.Segment(code.ram, code.data, "loaded")


def volatile_literals(prog):
    """Literal-pool slots that some literal points at: data the code may
    write, so their loads read memory instead of folding the constant."""
    pools, values = {}, set()
    mem = prog.mem
    for f in prog.funcs.values():
        for a in f.code:
            ins = mem.insn(a, f.thumb)
            size = ins.literal_size
            if size:
                pools[ins.target] = size
                if size == 4:
                    v = mem.u32(ins.target)
                    if v is not None:
                        values.add(v)
    out = set()
    for v in values:
        for t in (v, v & ~1, v & ~3):
            if t in pools and t <= v < t + pools[t]:
                out.add(t)
    return out


class Module:
    """A program's (or one overlay's) functions to C++.

    `overlay`: (id, segment) when the module is one overlay of the program: its
    discovery runs over every segment with the overlay loaded, but only the
    functions inside the overlay's segment are emitted; `ext` (namespace, keys)
    names the program's module, whose entries it calls directly. `prog` may be
    given, already discovered."""

    def __init__(self, name, segs, cpu, seeds, overlay=None, ext=None, prog=None, loaded=None):
        self.name, self.segs, self.cpu = name, segs, cpu
        self.loaded = loaded                 # the program as loaded, when it decompresses itself
        self.ns = "p_" + name.lower()
        self.overlay, self.ext = overlay, ext
        if prog is None:
            mem = discover.Memory(segs, arch=5 if cpu == 9 else 4)
            prog = discover.Program(mem, seeds).run()
        self.prog = prog
        own = prog.funcs
        if overlay is not None:
            seg = overlay[1]
            own = {e: f for e, f in prog.funcs.items() if seg.base <= e < seg.end}
        self.own = own
        self.entries = {e | int(f.thumb): f.thumb for e, f in own.items()}
        self.names = {}
        if ext is not None:
            ens, keys = ext
            for k in keys:
                if k not in self.entries:
                    self.entries[k] = bool(k & 1)
                    self.names[k] = "%s::%s" % (ens, E.fname(k & ~1, k & 1))
        self.files, self.sites, self.unknown = [], {}, []
        self.n_ins = 0

    def extent(self, f, volatile):
        """The bytes a function of an overlay is built from: its first instruction
        to its last, and the literal pools it reads constants from (folded into
        the code), within the overlay's segment."""
        seg, mem = self.overlay[1], self.prog.mem
        lo = min(f.code)
        hi = max(f.code) + (2 if f.thumb else 4)
        for a in f.code:
            ins = mem.insn(a, f.thumb)
            n = ins.literal_size
            if n and ins.target not in volatile and seg.base <= ins.target and ins.target + n <= seg.end:
                lo, hi = min(lo, ins.target), max(hi, ins.target + n)
        return max(lo, seg.base), min(hi, seg.end)

    def generate(self, out, per_file, comments):
        prog = self.prog
        volatile = volatile_literals(prog)
        self.volatile = len(volatile)
        buf, count, idx = [], 0, 0
        arch = 5 if self.cpu == 9 else 4

        def flush():
            nonlocal buf, count, idx
            if not buf:
                return
            name = "%s_%03d.cpp" % (self.ns, idx)
            with Out(os.path.join(out, name)) as f:
                f.write('#include "%s_funcs.h"\n\nnamespace %s {\nusing namespace nds%d;\n\n'
                        % (self.ns, self.ns, self.cpu))
                f.write("\n\n".join(buf))
                f.write("\n\n}  // namespace %s\n" % self.ns)
            self.files.append(name)
            buf, count, idx = [], 0, idx + 1

        self.interpreted = []
        for e in sorted(self.own):
            fn = self.own[e]
            body = E.Body(prog, fn, self.entries, volatile, arch, comments, self.names)
            try:
                lines = body.emit()
            except E.Unsupported as why:     # bytes that are not code as they lie (encrypted, data)
                self.interpreted.append((e, str(why)))
                buf.append("\n".join(E.interpreted(fn.entry, fn.thumb, str(why))))
                count += len(fn.code)
                continue
            buf.append("\n".join(lines))
            for k, v in body.sites.items():
                self.sites[k] = self.sites.get(k, 0) + v
            self.unknown += [(t, e) for t in body.unknown]
            count += len(fn.code)
            self.n_ins += len(fn.code)
            if count >= per_file:
                flush()
        flush()

        keys = sorted(e | int(f.thumb) for e, f in self.own.items())
        with Out(os.path.join(out, self.ns + "_funcs.h")) as f:
            f.write('#pragma once\n#include "arm.h"\n')
            if self.ext is not None:
                f.write('#include "%s_funcs.h"\n' % self.ext[0])
            f.write('\nnamespace %s {\n' % self.ns)
            for k in keys:
                f.write("void %s(ArmContext& c);\n" % E.fname(k & ~1, k & 1))
            f.write("extern const ArmModule module;\n}  // namespace %s\n" % self.ns)
        segs = [self.overlay[1]] if self.overlay is not None else self.segs
        with Out(os.path.join(out, self.ns + "_table.cpp")) as f:
            f.write('#include "%s_funcs.h"\n\nnamespace %s {\n\n' % (self.ns, self.ns))
            f.write("static const ArmSegment segs[] = {\n")
            for s in segs:
                kind = 2 if self.overlay is not None else 1 if s.name == "copy" else 0
                f.write("    {0x%08Xu, 0x%Xu, 0x%08Xu, %d},  // %s\n" % (
                    s.base, len(s.data), zlib.crc32(s.data), kind, s.name))
            if self.loaded is not None:
                s = self.loaded
                f.write("    {0x%08Xu, 0x%Xu, 0x%08Xu, 3},  // as loaded, compressed\n" % (
                    s.base, len(s.data), zlib.crc32(s.data)))
            f.write("};\n\nstatic const ArmFuncEntry funcs[] = {\n")
            for k in keys:
                f.write("    {0x%08Xu, %s},\n" % (k, E.fname(k & ~1, k & 1)))
            f.write("};\n\n")
            land = sorted({r | int(prog.funcs[e].thumb) for e, rs in prog.landings.items() if e in self.own
                           for r in rs})
            if land:
                f.write("static const uint32_t landings[] = {%s};\n\n" % ", ".join("0x%08Xu" % t for t in land))
            tail = "-1, nullptr, nullptr"
            if self.overlay is not None:
                s = self.overlay[1]
                ext = [self.extent(self.own[k & ~1], volatile) for k in keys]
                f.write("static const ArmRange extents[] = {\n%s};\n\n" % "".join(
                    "    {0x%08Xu, 0x%Xu},\n" % (lo, hi - lo) for lo, hi in ext))
                f.write("static const uint32_t crcs[] = {\n%s};\n\n" % "".join(
                    "    0x%08Xu,\n" % zlib.crc32(s.data[lo - s.base:hi - s.base]) for lo, hi in ext))
                tail = "%d, extents, crcs" % self.overlay[0]
            f.write('extern const ArmModule module = {"%s", %d, segs, %d, funcs, %d, %s, %d, %s};\n'
                    % (self.name, self.cpu, len(segs) + (self.loaded is not None), len(keys), "landings" if land else "nullptr",
                       len(land), tail))
            f.write("\n}  // namespace %s\n" % self.ns)
        self.files.append(self.ns + "_table.cpp")


CMAKE = """cmake_minimum_required(VERSION 3.20)
project(ndskit_recomp CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(NDSKIT_RUNTIME "{rt}")
add_library(recomp STATIC
    {srcs})
target_include_directories(recomp PUBLIC ${{NDSKIT_RUNTIME}})
target_compile_options(recomp PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-tautological-compare)
# the runtime: ndskit_core (memory, modes, dispatch over the modules) and
# ndskit_stub (no hardware: tests that only run the game's own code)
include(${{NDSKIT_RUNTIME}}/runtime.cmake)
add_executable(selftest ${{NDSKIT_RUNTIME}}/selftest.cpp)
target_link_libraries(selftest ndskit_core ndskit_stub recomp)
# the machine, headless: a cartridge's programs run (ndskit/runtime/main.cpp)
add_executable(ndsrun ${{NDSKIT_RUNTIME}}/main.cpp)
target_link_libraries(ndsrun ndskit_core ndskit_machine recomp)
# a project can add its own targets (the game) with -DNDSKIT_EXTRA=file.cmake
if(DEFINED NDSKIT_EXTRA)
  include(${{NDSKIT_EXTRA}})
endif()
"""


def generate(modules_spec, out, per_file=8000, comments=True, optest=False, log=print):
    """modules_spec: [(name, segments, cpu, seeds[, Module's keyword arguments])]."""
    t0 = time.time()
    os.makedirs(out, exist_ok=True)
    specs = list(modules_spec)
    if optest:
        from . import selftest
        for cpu in (9, 7):
            name, segs, seeds = selftest.write_optest(os.path.join(out, "selftest"), cpu)
            specs.append((name, segs, cpu, seeds))
    modules = []
    for spec in specs:
        name, segs, cpu, seeds = spec[:4]
        t = time.time()
        m = Module(name, segs, cpu, seeds, **(spec[4] if len(spec) > 4 else {}))
        m.generate(out, per_file, comments)
        modules.append(m)
        log("%-9s %5d functions %8d instructions %3d files  %.1f s"
            % (name, len(m.own), m.n_ins, len(m.files), time.time() - t))
    with Out(os.path.join(out, "modules.cpp")) as f:
        for m in modules:
            f.write('#include "%s_funcs.h"\n' % m.ns)
        f.write("\nextern const ArmModule* const g_arm_modules[] = {\n")
        for m in modules:
            f.write("    &%s::module,\n" % m.ns)
        f.write("};\nextern const int g_arm_nmodules = %d;\n" % len(modules))
    srcs = [x for m in modules for x in m.files] + ["modules.cpp"]
    with Out(os.path.join(out, "CMakeLists.txt")) as f:
        f.write(CMAKE.format(rt=RUNTIME.replace("\\", "/"), srcs="\n    ".join(srcs)))
    hows = ["resolved", "unresolved", "switch", "tail", "unresolved jump"]
    rows = ["%-9s %6s %8s  %s  %8s  %s" % ("module", "funcs", "insns", " ".join("%10s" % h[:10] for h in hows),
                                          "volatile", "targets not entries")]
    for m in modules:
        rows.append("%-9s %6d %8d  %s  %8d  %d" % (
            m.name, len(m.own), m.n_ins, " ".join("%10d" % m.sites.get(h, 0) for h in hows), m.volatile,
            len({t for t, _ in m.unknown})))
    tot = ("total: %d modules, %d functions, %d instructions, %d files, %.1f s"
           % (len(modules), sum(len(m.own) for m in modules), sum(m.n_ins for m in modules),
              len(srcs), time.time() - t0))
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as f:
        f.write("transfers through registers: calls (resolved: compared with the target analysis\n"
                "expects; unresolved: arm_call), switches, jumps to a known target, jumps left to\n"
                "arm_jump at run time:\n")
        f.write("\n".join(rows) + "\n" + tot + "\n\n")
        for m in modules:
            for e, why in m.interpreted:
                f.write("%s: %08X left to the interpreter (%s)\n" % (m.name, e, why))
            seen = set()
            for t, e in sorted(m.unknown):
                if t not in seen:
                    seen.add(t)
                    f.write("%s: target %08X (from %08X) is not an entry of %s\n" % (m.name, t, e, m.name))
    log(tot)
    return modules


def program_specs(rom, cpu, segs, seeds, overlays, log=print):
    """A program's module and one module per overlay. Each overlay is
    discovered with the program, loaded at its address; the program's module
    takes every function of its own that some run found (those only its
    overlays call included), and each overlay's module calls them directly."""
    name = "ARM%d" % cpu
    if not overlays:
        return [(name, segs, cpu, seeds)]
    arch = 5 if cpu == 9 else 4

    def discover_one(o, oseg, extra=()):
        mem = discover.Memory(segs + [oseg], arch=arch)
        prog = discover.Program(mem, seeds + discover.overlay_seeds(rom, cpu, o.id, mem))
        prog.foreign |= set(extra)           # taken where they walk clean and start as a function would
        prog.areas = discover.overlay_areas(rom, cpu, o.id)
        return prog.run()

    segments = {o.id: discover.Segment(o.ram, discover.overlay_data(rom, o), "ov%d" % o.id, o.id)
                for o in overlays}
    progs = {}
    for o in overlays:
        t = time.time()
        progs[o.id] = discover_one(o, segments[o.id])
        oseg = segments[o.id]
        log("overlay %d: %d functions discovered with the program  %.1f s"
            % (o.id, sum(1 for e in progs[o.id].funcs if oseg.base <= e < oseg.end), time.time() - t))
    # an overlay calling into another slot's overlay (a battle's calling the
    # field's): the targets, outside that run's memory, seed the overlays of
    # that area again, each taking those that walk clean in it
    extra = {}

    def foreign(o, c, ct):
        for p in overlays:
            s = segments[p.id]
            if p.id != o.id and s.base <= c < s.end:
                extra.setdefault(p.id, set()).add((c, ct))

    for o in overlays:
        oseg, prog = segments[o.id], progs[o.id]
        for e, f in prog.funcs.items():
            if not oseg.base <= e < oseg.end:
                continue
            for c, ct in f.calls | f.tails:
                if not prog.mem.inside(c):
                    foreign(o, c, ct)
        # pointers in the overlay's data and literal pools (a vtable of a class whose
        # methods another overlay holds, a callback): code addresses there too
        for a in range(oseg.base, oseg.end - 3, 4):
            if a in prog.code:
                continue
            v = prog.mem.u32(a)
            if v and (v & 1 or not v & 3) and not prog.mem.inside(v & ~1):
                foreign(o, v & ~1, bool(v & 1))
    for pid, targets in sorted(extra.items()):
        p = next(x for x in overlays if x.id == pid)
        t = time.time()
        progs[pid] = discover_one(p, segments[pid], targets)
        log("overlay %d: discovered again with %d targets other overlays call  %.1f s"
            % (pid, len(targets), time.time() - t))
    runs, program = [], set()
    for o in overlays:
        oseg, prog = segments[o.id], progs[o.id]
        mine = [e for e in prog.funcs if oseg.base <= e < oseg.end]
        program |= {e | int(f.thumb) for e, f in prog.funcs.items() if not oseg.base <= e < oseg.end}
        if mine:
            runs.append((o, oseg, prog))
    out = [(name, segs, cpu, sorted(set(seeds) | program))]
    ns = "p_" + name.lower()
    for o, oseg, prog in runs:
        out.append(("%s_OV%d" % (name, o.id), segs + [oseg], cpu, [],
                    dict(overlay=(o.id, oseg), ext=(ns, program), prog=prog)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rom", nargs="?", help="a cartridge (.nds)")
    ap.add_argument("--cpu", default="9,7", help="which programs of the cartridge (9, 7 or 9,7)")
    ap.add_argument("--image", action="append", default=[], help="NAME=FILE@BASE:CPU[+SEED,...]")
    ap.add_argument("--copy", action="append", default=[],
                    help="CPU:DEST=SRC:LEN (hex): code the program copies at run time, recompiled where it runs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-file", type=int, default=8000)
    ap.add_argument("--no-comments", action="store_true")
    ap.add_argument("--optest", action="store_true", help="add ndskit's instruction test modules")
    ap.add_argument("--no-overlays", action="store_true", help="the programs alone, without their overlays")
    a = ap.parse_args(argv)
    specs = []
    if a.rom:
        from ..rom import Rom
        with open(a.rom, "rb") as fh:
            rom = Rom(fh.read())
        for cpu in (int(x) for x in a.cpu.split(",") if x):
            segs, entry = discover.segments_from_rom(rom, cpu)
            seeds = [entry]
            for c, dest, src, n in (parse_copy(x) for x in a.copy):
                if c == cpu:
                    segs.append(copy_segment(segs, dest, src, n))
                    seeds.append(dest)
            overlays = [] if a.no_overlays else (rom.overlays9 if cpu == 9 else rom.overlays7)
            ps = program_specs(rom, cpu, segs, seeds, overlays)
            ps[0] = ps[0][:4] + (dict(loaded=loaded_segment(rom, cpu)),)
            specs += ps
    for spec in a.image:
        name, path, base, cpu, seeds = parse_image(spec)
        with open(path, "rb") as fh:
            data = fh.read()
        specs.append((name, [discover.Segment(base, data, "main")], cpu, [base] + seeds))
    generate(specs, a.out, a.per_file, not a.no_comments, a.optest)


if __name__ == "__main__":
    main()
