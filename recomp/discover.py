"""Function discovery for DS programs without symbols (ARM and Thumb).

    python -m ndskit.recomp.discover GAME.nds --cpu 9 [--seeds a,b,..] [--report] [--out funcs.tsv]
    python -m ndskit.recomp.discover FILE.bin --base 02000000 [--entry 02000800] ...

A DS program is a flat image whose code, literal pools, switch tables and
data share the text. This is a recursive descent that classifies words as
code or data while it finds the functions, written for CodeWarrior's
output (the compiler of NitroSDK games) and the SDK's hand-written parts:

1. Segments. A cartridge's program is read where it runs: the main block
   at its load address up to its autoload sources, and each autoload block
   (ITCM, DTCM, the ARM7's WRAM) at its destination (`ndskit.rom`'s
   ModuleParams). An address belongs to one segment.
2. Seeds: the entry point, the caller's list, and every call target found
   on the way. A function has a mode, ARM or Thumb; `bl` keeps it, `blx`
   (immediate) changes it, and a target taken from a value (a literal, a
   pointer in data) is Thumb when its bit 0 is set.
3. A function is the code reachable from its entry: conditional branches
   both ways, `b`, switch tables, fall-through, conditional instructions
   (including conditional returns) running on. A `b` to another
   function's entry is a tail call and is not followed. Returns: `bx lr`,
   `mov pc, lr`, `pop`/`ldm` with pc.
4. Constants are propagated through a straight line of code (`ldr` from
   the literal pool, `mov`/`mvn` of an immediate, `add`/`sub` from pc,
   register copies; a call clobbers r0-r3, r12 and lr). They resolve
   `bx rN`, `blx rN` and `mov pc, rN` whose rN holds a known address:
   CodeWarrior's long-branch veneers (`ldr r12, [pc]; bx r12`) and calls
   through a register. `mov lr, pc` (or `add lr, pc, #n`) before a jump
   through a register, or an address put in lr earlier in the same
   straight line (`sub lr, pc, #n` at a dispatch loop's head), makes it a
   call: execution resumes at lr's value. Two conditional transfers in a
   row with opposite conditions on the same flags (`bxpl r12` then
   `popmi {..., pc}`) end the block: nothing runs after them.
5. Literal loads mark their bytes as data.
6. Switches. CodeWarrior's ARM switch: `cmp rI, #N` ... `addls pc, pc,
   rI, lsl #2`, then the default's `b` and N + 1 entries, each one
   instruction (a `b` to the case) walked as code. The jump through a
   table of offsets relative to the table (hand-written code: `sub rT,
   pc, #k` or `add rT, pc, #k`; `ldr rX, [rT, rI, lsl #2]`; `add pc, rX,
   rT`): entries are read while each lands on code inside the segment,
   after the table; with lr set first (`add lr, pc, #n`) it is a call.
7. Unreached code: after the pass, a word in the data that points into a
   segment is a function reached by pointer (a callback, a handler, a
   virtual method) if its descent meets no undefined instruction and no
   data, and it either opens with a stack-frame prologue (`push {..., lr}`
   or `stmdb sp!`), is loaded by the code as a literal, or sits in a run of
   at least two such pointers (a table). Failing that, unclassified words
   right after a function's end or a literal pool that open with a
   prologue are taken the same way (the linker places whole object files,
   so uncalled functions sit between called ones). Repeat to a fixed point.

`Program` keeps: funcs {entry: Function}, code {address: size}, data
(byte addresses), switches {jump address: [targets]}, the calls and jumps
through registers it could not resolve, and the descents that met data or
an undefined instruction (conflicts).

Checked on Moon's ARM9 against Ghidra 12's auto-analysis (ndskit/ghidra):
of Ghidra's 4 680 functions, 4 646 found as entries and 17 inside another
function's code (entries a few bytes apart, shared tails), 17 not reached
(15 of them hand-written decoder routines reached through tables of
offsets whose base is in a register set elsewhere); 749 more found that
Ghidra has not. Its ARM7: 535 of Ghidra's 536 (the program read where its
autoloads run), 390 more. No conflict on Moon or two other cartridges.
Known gap: CodeWarrior's Thumb switches are not recognised yet (a
Thumb-heavy 2008 cartridge leaves half its ARM9 unclassified).
"""
import argparse
import bisect
import collections
import struct

from .. import arm


class Segment:
    def __init__(self, base, data, name=""):
        self.base, self.data, self.name = base, data, name
        self.end = base + len(data)
        self.img = arm.Image(data, base)


class Memory:
    """The program's segments, read at their run addresses."""

    def __init__(self, segments, arch=5):
        self.segs = sorted(segments, key=lambda s: s.base)
        self.arch = arch
        for s in self.segs:
            s.img.arch = arch
        self._starts = [s.base for s in self.segs]
        self._cache = {}

    def seg(self, a):
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0 and a < self.segs[i].end:
            return self.segs[i]
        return None

    def inside(self, a, n=1):
        s = self.seg(a)
        return s is not None and a + n <= s.end

    def u32(self, a):
        s = self.seg(a)
        if s is None or a + 4 > s.end:
            return None
        return struct.unpack_from("<I", s.data, a - s.base)[0]

    def u16(self, a):
        s = self.seg(a)
        if s is None or a + 2 > s.end:
            return None
        return struct.unpack_from("<H", s.data, a - s.base)[0]

    def insn(self, pc, thumb):
        key = (pc, thumb)
        ins = self._cache.get(key)
        if ins is None:
            s = self.seg(pc)
            if s is None or pc + (2 if thumb else 4) > s.end:
                return None
            if thumb:
                nxt = self.u16(pc + 2)
                ins = arm.decode_thumb(self.u16(pc), pc, nxt, self.arch)
            else:
                ins = arm.decode_arm(self.u32(pc), pc, self.arch)
            self._cache[key] = ins
        return ins


class Function:
    __slots__ = ("entry", "thumb", "code", "calls", "tails", "conflicts", "unresolved", "size")

    def __init__(self, entry, thumb):
        self.entry, self.thumb = entry, thumb
        self.code = set()          # instruction addresses
        self.calls = set()         # (target, thumb)
        self.tails = set()
        self.conflicts = []        # (address, why)
        self.unresolved = []       # (address, text)
        self.size = 0


def _is_prologue(ins, mem=None):
    """push {..., lr}; or, given mem, a variadic function's push {r0-r3} before one."""
    if ins is None or ins.op != "stm" or ins.rn != 13 or not ins.writeback or ins.mode != "db" \
            or not ins.reglist:
        return False
    if ins.reglist & 0x4000:
        return True
    if mem is not None and not ins.reglist & ~0xF:
        return _is_prologue(mem.insn(ins.pc + ins.size, ins.thumb))
    return False


def _clobbers(ins):
    """Registers whose constant an instruction destroys."""
    out = set()
    if ins.rd is not None and ins.op not in arm.COMPARES and ins.op not in arm.STORES:
        out.add(ins.rd)
    if ins.op in arm._LONG_MULS and ins.rd2 is not None:
        out.add(ins.rd2)
    if ins.op == "ldrd" and ins.rd is not None:
        out.update((ins.rd, ins.rd + 1))
    if ins.op == "ldm" and ins.reglist:
        out.update(r for r in range(16) if ins.reglist >> r & 1)
    if ins.writeback and ins.rn is not None:
        out.add(ins.rn)
    if ins.is_call:
        out.update((0, 1, 2, 3, 12, 14))
    return out


class Program:
    def __init__(self, mem, seeds=()):
        self.mem = mem
        self.funcs = {}
        self.code = {}             # address -> size
        self.data = set()          # byte addresses
        self.switches = {}
        self.literal_loaded = set()    # values loaded by code from literal pools
        self.seeds = [(a & ~1, bool(a & 1)) if isinstance(a, int) else a for a in seeds]
        self.entries = {}          # address -> thumb, every known entry

    # ---- one function -------------------------------------------------
    def _consts_step(self, regs, ins):
        """Update the straight-line constants after ins; regs is a dict."""
        val = None
        if ins.op == "ldr" and ins.rn == 15 and ins.target is not None and ins.cond == 14:
            val = self.mem.u32(ins.target)
        elif ins.op in ("mov", "mvn") and ins.imm is not None and ins.rm is None and ins.cond == 14:
            val = ins.imm & 0xFFFFFFFF if ins.op == "mov" else ~ins.imm & 0xFFFFFFFF
        elif ins.op in ("add", "sub") and ins.rn == 15 and ins.target is not None and ins.cond == 14:
            val = ins.target
        elif ins.op == "mov" and ins.rm is not None and ins.imm is None and ins.shift_imm == 0 \
                and ins.rs is None and ins.cond == 14 and ins.rm in regs:
            val = regs[ins.rm]
        for r in _clobbers(ins):
            regs.pop(r, None)
        if val is not None and ins.rd is not None:
            regs[ins.rd] = val

    def _walk(self, f, commit):
        """Descend one function: its code, calls, tail calls, conflicts, unresolved jumps."""
        thumb = f.thumb
        mem = self.mem
        todo = [(f.entry, {})]
        seen = set()
        while todo:
            pc, regs = todo.pop()
            regs = dict(regs)
            ended = None    # condition of the last conditional transfer, while the flags hold
            while True:
                if pc in seen:
                    break
                if pc != f.entry and self.entries.get(pc) == thumb:
                    f.tails.add((pc, thumb))
                    break
                ins = mem.insn(pc, thumb)
                if ins is None:
                    f.conflicts.append((pc, "outside the program"))
                    break
                if ins.op in ("undefined", "bl_hi", "bl_lo", "blx_lo"):
                    f.conflicts.append((pc, "undefined instruction"))
                    break
                if any(b in self.data for b in range(pc, pc + ins.size)):
                    f.conflicts.append((pc, "runs into data"))
                    break
                seen.add(pc)
                f.code.add(pc)
                lit = ins.literal_size
                if lit:
                    if commit:
                        self.data.update(range(ins.target, ins.target + lit))
                    if lit == 4:
                        v = mem.u32(ins.target)
                        if v is not None:
                            self.literal_loaded.add(v)
                src = regs.get(ins.rm) if ins.rm is not None else None
                lr_before = regs.get(14)
                self._consts_step(regs, ins)
                nxt = pc + ins.size
                op, cond = ins.op, ins.cond
                # a conditional transfer falls through, unless the one before it had
                # the opposite condition on the same flags (bxpl ...; popmi {..., pc})
                go_on = cond != 14 and not (ended is not None and cond == ended ^ 1)
                if ins.s or op in arm.COMPARES:
                    ended = None
                if op in ("bl", "blx") and ins.target is not None:
                    f.calls.add((ins.target, ins.exchange == "thumb" if ins.exchange else thumb))
                    pc = nxt
                    continue
                if op == "blx":             # through a register
                    if src is not None:
                        f.calls.add((src & ~1, bool(src & 1)))
                    else:
                        f.unresolved.append((pc, "call " + ins.text))
                    pc = nxt
                    continue
                if op == "b":
                    if not go_on:
                        if self.entries.get(ins.target) == thumb and ins.target != f.entry:
                            f.tails.add((ins.target, thumb))
                            break
                        pc = ins.target
                        continue
                    todo.append((ins.target, regs))
                    ended = cond
                    pc = nxt
                    continue
                if not ins.writes_pc:
                    pc = nxt
                    continue
                # everything else that writes pc
                if op == "bx" and ins.rm == 14 or op == "mov" and ins.rm == 14 and ins.imm is None \
                        or op == "ldm" or op == "ldr" and ins.rn == 13 and ins.writeback:
                    if go_on:                   # a conditional return
                        ended = cond
                        pc = nxt
                        continue
                    break
                sw = self._switch(f, ins, regs, commit)
                if sw is not None:
                    todo.extend((t, {}) for t in sw)
                    if go_on:
                        pc = nxt
                        continue
                    break
                if op in ("bx", "mov") and ins.imm is None:
                    target = src
                elif op == "ldr" and ins.rn == 15 and ins.target is not None:
                    target = mem.u32(ins.target)
                else:
                    target = None
                lr = self._lr_set(pc, thumb)
                if lr is None and lr_before is not None and mem.inside(lr_before & ~1):
                    lr = lr_before          # lr set further back in the straight line
                kind = "call " if lr is not None else "jump "
                if target is not None:
                    (f.calls if lr is not None else f.tails).add((target & ~1, bool(target & 1)))
                else:
                    f.unresolved.append((pc, kind + ins.text))
                if lr is not None:
                    if lr != nxt:
                        todo.append((lr, {}))
                        if not go_on:
                            break
                        ended = cond
                    pc = nxt
                    continue
                if go_on:
                    ended = cond
                    pc = nxt
                    continue
                break
        f.size = sum(mem.insn(a, thumb).size for a in f.code)
        return f

    def _lr_set(self, pc, thumb):
        """If the instruction before pc set lr to a return address (mov lr, pc /
        add lr, pc, #n), that address; else None."""
        p = self.mem.insn(pc - (2 if thumb else 4), thumb)
        if p is None or p.rd != 14 or p.cond != 14:
            return None
        if p.op == "mov" and p.rm == 15 and p.imm is None and p.shift_imm == 0:
            return p.pc + 8
        if p.op in ("add", "sub") and p.rn == 15 and p.target is not None:
            return p.target
        return None

    def _switch(self, f, ins, regs, commit):
        """Targets of a recognised computed jump, or None."""
        mem, thumb = self.mem, f.thumb
        # CodeWarrior: [cmp rI, #N] addls pc, pc, rI, lsl #2 ; b default ; N+1 entries
        if not thumb and ins.op == "add" and ins.rd == 15 and ins.rn == 15 and ins.rm is not None \
                and ins.shift == "lsl" and ins.shift_imm == 2 and ins.rs is None:
            n = self._bound(ins.pc, ins.rm, thumb)
            if n is None:
                return None
            first = ins.pc + 8
            targets = [ins.pc + 4] + [first + 4 * k for k in range(n + 1)]
            self.switches[ins.pc] = targets
            return targets
        # offsets relative to a table: ldr rX, [rT, rI, lsl #2] ; add pc, rX, rT
        if not thumb and ins.op == "add" and ins.rd == 15 and ins.rm is not None and ins.rs is None \
                and ins.shift_imm == 0:
            for back in range(1, 4):
                p = mem.insn(ins.pc - 4 * back, False)
                if p is not None and p.op == "ldr" and p.rd in (ins.rn, ins.rm) and p.rm is not None \
                        and p.shift == "lsl" and p.shift_imm == 2:
                    base_reg = ins.rm if p.rd == ins.rn else ins.rn
                    table = regs.get(base_reg)
                    if table is None or p.rn != base_reg:
                        return None
                    targets = []
                    k = 0
                    limit = None
                    while k < 1024:
                        off = mem.u32(table + 4 * k)
                        if off is None:
                            break
                        t = (table + off) & 0xFFFFFFFF
                        if limit is not None and table + 4 * k >= limit:
                            break
                        if not mem.inside(t, 4) or t & 3 or t <= table + 4 * k:
                            break
                        ti = mem.insn(t, False)
                        if ti is None or ti.op == "undefined":
                            break
                        targets.append(t)
                        limit = t if limit is None else min(limit, t)
                        k += 1
                    if not targets:
                        return None
                    if commit:
                        self.data.update(range(table, table + 4 * len(targets)))
                    lr = self._lr_set(ins.pc, False)
                    if lr is not None:
                        targets = targets + [lr]
                    self.switches[ins.pc] = targets
                    return targets
        return None

    def _bound(self, at, reg, thumb):
        """N from the `cmp reg, #N` that guards a switch (within a few instructions)."""
        step = 2 if thumb else 4
        for back in range(1, 6):
            p = self.mem.insn(at - step * back, thumb)
            if p is None:
                return None
            if p.op == "cmp" and p.rn == reg and p.imm is not None:
                return p.imm
            if p.rd == reg or p.writes_pc:
                return None
        return None

    # ---- the whole program ---------------------------------------------
    def _descend_all(self, commit):
        self.funcs = {}
        work = list(self.entries.items())
        while work:
            a, t = work.pop()
            if a in self.funcs:
                continue
            f = Function(a, t)
            self._walk(f, commit)
            self.funcs[a] = f
            for (c, ct) in f.calls | f.tails:
                if self.mem.inside(c) and c not in self.entries:
                    self.entries[c] = ct
                    work.append((c, ct))

    def run(self, max_rounds=12):
        for a, t in self.seeds:
            self.entries[a] = t
        last = -1
        for _ in range(max_rounds):
            # calls and tails grow the set of entries; walk until it is stable
            while True:
                n = len(self.entries)
                self.data = set()
                self._descend_all(commit=True)
                if len(self.entries) == n:
                    break
            self.code = {}
            for f in self.funcs.values():
                for a in f.code:
                    self.code[a] = 2 if f.thumb else 4
            new = self._pointer_seeds() | self._prologue_seeds()
            new = {(a, t) for a, t in new if a not in self.entries}
            if not new or len(self.entries) == last:
                break
            last = len(self.entries)
            for a, t in new:
                self.entries[a] = t
        return self

    def _clean_descent(self, a, thumb, minimum=1):
        f = Function(a, thumb)
        saved = dict(self.entries)
        self._walk(f, commit=False)
        self.entries = saved
        if f.conflicts or len(f.code) < minimum:
            return False
        if any(c in self.code for c in f.code):
            return False
        return True

    def _data_words(self):
        """Word-aligned addresses that are data or unclassified, inside the segments."""
        for s in self.mem.segs:
            for a in range(s.base, s.end - 3, 4):
                if a not in self.code and (a + 2) not in self.code:
                    yield a

    def _pointer_seeds(self):
        cand = {}
        for a in self._data_words():
            v = self.mem.u32(a)
            if v is None:
                continue
            t, thumb = v & ~1, bool(v & 1)
            if not self.mem.inside(t, 2) or (not thumb and t & 3) or t in self.entries:
                continue
            if t in self.code or t in self.data:
                continue
            cand.setdefault((t, thumb), []).append(a)
        out = set()
        for (t, thumb), places in cand.items():
            ins = self.mem.insn(t, thumb)
            if ins is None:
                continue
            v = t | thumb
            in_table = any(self._pointer_neighbour(p) for p in places)
            if _is_prologue(ins, self.mem):
                ok = self._clean_descent(t, thumb)
            elif v in self.literal_loaded or in_table or self._boundary(t, thumb):
                ok = self._clean_descent(t, thumb, minimum=4)
            else:
                ok = False
            if ok:
                out.add((t, thumb))
        return out

    def _boundary(self, a, thumb):
        """a follows data, or the last instruction of code that does not run on."""
        prev = a - (2 if thumb else 4)
        if prev in self.data:
            return True
        if prev in self.code:
            pi = self.mem.insn(prev, thumb)
            return pi is not None and pi.writes_pc and pi.cond == 14 and not pi.is_call
        return False

    def _pointer_neighbour(self, p):
        for q in (p - 4, p + 4):
            v = self.mem.u32(q)
            if v is not None and self.mem.inside(v & ~1, 2) and ((v & 1) or not v & 3):
                ins = self.mem.insn(v & ~1, bool(v & 1))
                if ins is not None and (_is_prologue(ins) or (v & ~1) in self.entries):
                    return True
        return False

    def _prologue_seeds(self):
        out = set()
        for s in self.mem.segs:
            for a in range(s.base, s.end - 3, 4):
                if a in self.code or a in self.entries:
                    continue
                prev = a - 4
                if not (prev in self.data or prev in self.code):
                    continue
                if prev in self.code:
                    pi = self.mem.insn(prev, False)
                    if pi is None or not (pi.writes_pc and pi.cond == 14 and not pi.is_call):
                        continue
                ins = self.mem.insn(a, False)
                if _is_prologue(ins, self.mem) and self._clean_descent(a, False):
                    out.add((a, False))
        return out

    # ---- reports ----------------------------------------------------------
    def coverage(self):
        """(bytes of code, bytes of data, bytes unclassified) per segment."""
        rows = []
        for s in self.mem.segs:
            c = sum(n for a, n in self.code.items() if s.base <= a < s.end)
            d = sum(1 for a in self.data if s.base <= a < s.end)
            rows.append((s, c, d, len(s.data) - c - d))
        return rows

    def summary(self):
        nf = len(self.funcs)
        nt = sum(f.thumb for f in self.funcs.values())
        conf = [(f.entry, c) for f in self.funcs.values() for c in f.conflicts]
        unres = [(f.entry, u) for f in self.funcs.values() for u in f.unresolved]
        lines = ["%d functions (%d ARM, %d Thumb), %d instructions" % (
            nf, nf - nt, nt, sum(len(f.code) for f in self.funcs.values()))]
        for s, c, d, u in self.coverage():
            lines.append("  %-10s %08X-%08X  code %7d  data %7d  unclassified %7d bytes" % (
                s.name, s.base, s.end, c, d, u))
        lines.append("%d switches (%d targets); %d calls/jumps through registers unresolved; %d conflicts" % (
            len(self.switches), sum(len(t) for t in self.switches.values()), len(unres), len(conf)))
        return "\n".join(lines), conf, unres


def segments_from_rom(rom, cpu):
    """The program of one CPU of a cartridge, as segments at their run addresses."""
    code = rom.arm9 if cpu == 9 else rom.arm7
    params = rom.params if cpu == 9 else rom.params7
    data = code.plain
    if params is None:
        return [Segment(code.ram, data, "main")], code.entry
    end = params.autoload_start - code.ram
    segs = [Segment(code.ram, data[:end], "main")]
    from ..hw import name
    for addr, size, bss, src in params.autoloads:
        blk = data[src - code.ram:src - code.ram + size]
        segs.append(Segment(addr, blk, (name(addr, cpu) or "%08X" % addr).split()[0]))
    return segs, code.entry


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help=".nds (with --cpu) or a flat binary (with --base)")
    ap.add_argument("--cpu", type=int, choices=(7, 9), default=9)
    ap.add_argument("--base", help="flat binary: load address (hex)")
    ap.add_argument("--entry", help="flat binary: entry (hex; default the base)")
    ap.add_argument("--seeds", default="", help="more entries, hex, comma-separated (odd = Thumb)")
    ap.add_argument("--report", action="store_true", help="list conflicts and unresolved jumps")
    ap.add_argument("--out", help="write ENTRY<TAB>SIZE<TAB>A|T for every function")
    a = ap.parse_args(argv)
    with open(a.file, "rb") as fh:
        raw = fh.read()
    if a.base is None:
        from ..rom import Rom
        segs, entry = segments_from_rom(Rom(raw), a.cpu)
    else:
        base = int(a.base, 16)
        segs, entry = [Segment(base, raw, "main")], int(a.entry or a.base, 16)
    mem = Memory(segs, arch=5 if a.cpu == 9 else 4)
    seeds = [entry] + [int(x, 16) for x in a.seeds.split(",") if x]
    prog = Program(mem, seeds).run()
    text, conf, unres = prog.summary()
    print(text)
    if a.report:
        for e, (at, why) in conf[:200]:
            print("conflict  %08X in %08X: %s" % (at, e, why))
        for e, (at, what) in unres[:400]:
            print("unresolved %08X in %08X: %s" % (at, e, what))
    if a.out:
        with open(a.out, "w") as fh:
            for e in sorted(prog.funcs):
                f = prog.funcs[e]
                fh.write("%08X\t%d\t%s\n" % (e, f.size, "T" if f.thumb else "A"))


if __name__ == "__main__":
    main()
