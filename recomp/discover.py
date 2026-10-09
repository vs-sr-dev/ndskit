"""Function discovery for DS programs without symbols (ARM and Thumb).

    python -m ndskit.recomp.discover GAME.nds --cpu 9 [--seeds a,b,..] [--report] [--out funcs.tsv]
    python -m ndskit.recomp.discover GAME.nds --cpu 9 --overlay all --out "funcs_{id}.tsv"
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
   `mov pc, lr`, `pop`/`ldm` with pc. An address the function puts in lr
   itself (`mov lr, pc`, `add lr, pc, #n`) is walked too: code that runs
   when a local subroutine returns there.
4. Constants are propagated through a straight line of code (`ldr` from
   the literal pool, `mov`/`mvn` of an immediate, `add`/`sub` from pc or
   of an immediate to a constant, `mov rd, pc`, register copies; a call
   clobbers r0-r3, r12 and lr). They resolve `bx rN`, `blx rN` and `mov
   pc, rN` whose rN holds a known address: CodeWarrior's long-branch
   veneers (`ldr r12, [pc]; bx r12`), Thumb's `bx pc` into the ARM code
   that follows, calls through a register, targets built in two steps
   (`add r4, pc, #k; add r4, r4, #0x2600; movle pc, r4`). `mov lr, pc`
   (or `add lr, pc, #n`) before a jump through a register, or an address
   put in lr earlier in the same straight line (`sub lr, pc, #n` at a
   dispatch loop's head), makes it a call: execution resumes at lr's
   value. Two conditional transfers in a row with opposite conditions on
   the same flags (`bxpl r12` then `popmi {..., pc}`) end the block:
   nothing runs after them. That holds on a straight line only: when the
   walk applied it across an address that something also jumps to (a
   join, reached with other flags), the function is walked again without
   it from there. Conditional `add/sub rX, pc, #k` make rX one of several
   code addresses (`addhs r4, pc, #a; addlo r4, pc, #b; ...; bx r4`):
   each is taken as an entry.
5. Literal loads mark their bytes as data.
6. Switches. CodeWarrior's ARM switch: `cmp rI, #N` ... `addls pc, pc,
   rI, lsl #2`, then the default's `b` and N + 1 entries, each one
   instruction (a `b` to the case) walked as code; the bound is found
   back through the straight line, conditional branches out of it
   included, and through `subs rI, rX, #k` after `cmp rX, #N` (a range of
   cases from k, `addpl`); for `addge` after `cmp rI, #0` (the lower
   bound) the upper one further back (`cmp rI, #N; bgt default`). With
   no bound, a table of branches is read
   while its entries are branches, and an unconditional `add pc, pc, rI,
   lsl #2` into code that is not a table (the SDK's unrolled division)
   takes every instruction of the run it lands in. Tables of offsets from
   their own address (hand-written code): `ldr rX, [rT, rI{, lsl #2}]`
   with rT a constant, then `add pc, rT, rX`, or `add rY, rT, rX` ...
   `mov pc, rY` (`bx rY`), the registers followed through the straight
   line; entries are read while each lands on an instruction outside the
   table, before or after it, an offset of 0 being a hole (a code with no
   case); with lr set first (`add lr, pc, #n`) it is a call.
7. Unreached code: after the pass, a word in the data that points into a
   segment is a function reached by pointer (a callback, a handler, a
   virtual method) if its descent meets no undefined instruction and no
   data, its first instruction has no condition, and it either opens with
   a stack-frame prologue (`push {..., lr}` or `stmdb sp!`), or has two of
   three marks: loaded by the code as a literal, in a table (a neighbouring
   word points at a function, a prologue or a veneer), placed after data
   or a function's end (or after an unclassified word that is a return:
   C++'s tiny virtual methods sit in a row) -- then even a lone `bx lr`
   (an empty callback, an empty static constructor) is one; with one mark,
   four instructions at least, or in a table a veneer to a known entry
   (an interrupt table's `ldr r12, =handler; mov r0, #n; bx r12`). The
   table's mark spreads to a fixed point within the pass (a vtable of
   small methods), and a pointer of a table or a literal into code
   already walked inside another function, after a function's end, is
   an entry too (a thunk that adjusts `this` and branches into the method
   it shares). Failing that, unclassified words right after a function's
   end or a literal pool that open with a prologue are taken the same way
   (the linker places whole object files, so uncalled functions sit
   between called ones). Repeat to a fixed point.
   A trial descent (for a pointer, a prologue or a crossing) that runs
   into a literal it loads itself is refused: an odd pointer to a string
   passed to a formatter has two marks and decodes as Thumb for a while.
8. Overlays (`--overlay N`, or `all` one at a time): the program with
   overlay N loaded at its address as a segment of its own, its static
   initialisers (the pointers between its sinit start and end) as seeds.
   Other overlays share those addresses, so a call or a jump into the
   overlay from outside it (the main program's direct calls into an
   overlay's region) is not followed blindly: it is an entry only if its
   descent there is clean, and otherwise meant for another overlay. The
   program's own functions are the union over the runs (`--out` with
   `{id}`: one file per overlay with its functions, and `main` with the
   program's, those only overlays reach included).
9. Context saves: a function that stores an address of its own code to
   memory returns twice (NitroSDK's OS_SaveContext); the return addresses
   of the calls to it are landings, where a context loaded later comes
   back (`returns_twice`, `landings`; the recompiler unwinds to them).

`Program` keeps: funcs {entry: Function}, code {address: size}, data
(byte addresses), switches {jump address: [targets]}, the calls and jumps
through registers it could not resolve, and the descents that met data or
an undefined instruction (conflicts). Each Function keeps, for every
instruction that writes pc from a value, what the walk took it for
(`sites`: a return, a call with its return address, a switch, a jump with
its target when known): the recompiler (recomp.emit) works from them.

Checked on Moon's ARM9 against Ghidra 12's auto-analysis (ndskit/ghidra):
of Ghidra's 4 680 functions, 4 666 found as entries and 14 inside another
function's code (entries a few bytes apart, shared tails), none left;
6 580 functions in all, 1 914 that Ghidra has not (most of them C++'s
small virtual methods and thunks, reached through vtables). Its ARM7
(session 2): 535 of Ghidra's 536 (the program read where its autoloads
run). No conflict on Moon or two other cartridges; and the runtime ran
Moon from its boot into its first room on what this finds.
Known gaps: CodeWarrior's Thumb switches (`add pc, rI` after a `lsl`;
a Thumb-heavy 2008 cartridge leaves half its ARM9 unclassified), and the
switches through tables of halfword offsets in a newer SDK's ARM7 (`cmp
rI, #N; bhi default; add rI, pc, rI, lsl #1; ldrh rI, [rI, #4]; add pc,
pc, rI`). Overlays that the game compresses itself (the overlay table's
flag clear, the file in a format of the game's) are read as they are
stored.

Checked with overlays on a 2009 cartridge (14 ARM9 overlays in two
slots, 117 direct calls from the main program into them): 0 conflicts in
each; the program's functions, with those only overlays reach, cover
every function of Ghidra's that is reachable (4 090 of 4 101; the 11 left
a cluster of dead code that only calls itself).
"""
import argparse
import bisect
import collections
import struct

from .. import arm


class Segment:
    def __init__(self, base, data, name="", overlay=None):
        self.base, self.data, self.name = base, data, name
        self.overlay = overlay      # an overlay's id: other overlays share its addresses
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

    def crosses(self, frm, to):
        """to lies in an overlay and frm outside it: what frm's code reaches there
        may be another overlay's, loaded at the same address."""
        s = self.seg(to)
        return s is not None and s.overlay is not None and self.seg(frm) is not s

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
    __slots__ = ("entry", "thumb", "code", "calls", "tails", "conflicts", "unresolved", "size", "sites")

    def __init__(self, entry, thumb):
        self.entry, self.thumb = entry, thumb
        self.code = set()          # instruction addresses
        self.calls = set()         # (target, thumb)
        self.tails = set()
        self.conflicts = []        # (address, why)
        self.unresolved = []       # (address, text)
        self.size = 0
        # what each instruction that writes pc from a value was taken for:
        # address -> (kind, target, return address); kind "return", "call", "jump"
        # or "switch" (its targets in Program.switches); target: the address with
        # bit 0 for Thumb, or None when unknown; return address: where lr points
        # (a call, or a switch that is one), else None
        self.sites = {}


def _is_prologue(ins, mem=None):
    """push {..., lr}; or, given mem, a variadic function's push {r0-r3} before one."""
    if ins is None or ins.op != "stm" or ins.rn != 13 or not ins.writeback or ins.mode != "db" \
            or not ins.reglist or ins.cond != 14:
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
        self.crossing = set()      # (address, thumb) reached in an overlay from outside it

    # ---- one function -------------------------------------------------
    def _consts_step(self, regs, ins):
        """Update the straight-line constants after ins; regs is a dict: a
        register's number for a known value, ("w", r) for a word read from the
        table at that address, ("o", r) for that table's address plus such a
        word (a target taken from a table of offsets)."""
        val = sym = None
        plain = ins.cond == 14 and ins.shift is None and ins.rs is None
        if ins.op == "ldr" and ins.rn == 15 and ins.target is not None and ins.cond == 14:
            val = self.mem.u32(ins.target)
        elif ins.op in ("mov", "mvn") and ins.imm is not None and ins.rm is None and ins.cond == 14:
            val = ins.imm & 0xFFFFFFFF if ins.op == "mov" else ~ins.imm & 0xFFFFFFFF
        elif ins.op in ("add", "sub") and ins.rn == 15 and ins.target is not None and ins.cond == 14:
            val = ins.target
        elif ins.op in ("add", "sub") and ins.imm is not None and ins.rn in regs and ins.cond == 14:
            val = (regs[ins.rn] + (ins.imm if ins.op == "add" else -ins.imm)) & 0xFFFFFFFF
        elif ins.op == "mov" and ins.rm is not None and ins.imm is None and plain:
            if ins.rm == 15:
                val = ins.pc + (4 if ins.thumb else 8)
            elif ins.rm in regs:
                val = regs[ins.rm]
            elif ("o", ins.rm) in regs:
                sym = ("o", regs[("o", ins.rm)])
        elif ins.op == "ldr" and ins.rm is not None and ins.pre and ins.add and not ins.writeback \
                and ins.cond == 14 and ins.shift in (None, "lsl") and ins.shift_imm in (0, 2) \
                and ins.rn in regs:
            sym = ("w", regs[ins.rn])
        elif ins.op == "add" and ins.rm is not None and ins.imm is None and plain and not ins.s:
            for a, b in ((ins.rn, ins.rm), (ins.rm, ins.rn)):
                if a in regs and regs.get(("w", b)) == regs[a]:
                    sym = ("o", regs[a])
        # a conditional `add/sub rd, pc, #k`: rd is one of several code addresses
        # (`addhs r4, pc, #a; addlo r4, pc, #b; ...; bx r4`)
        alt = None
        if ins.op in ("add", "sub") and ins.rn == 15 and ins.target is not None and ins.cond != 14:
            alt = set(regs.get(("alt", ins.rd), ())) | {ins.target}
        for r in _clobbers(ins):
            regs.pop(r, None)
            regs.pop(("w", r), None)
            regs.pop(("o", r), None)
            regs.pop(("alt", r), None)
        if alt is not None:
            regs[("alt", ins.rd)] = alt
        if ins.rd is not None:
            if val is not None:
                regs[ins.rd] = val
            elif sym is not None:
                regs[(sym[0], ins.rd)] = sym[1]

    def _walk(self, f, commit):
        """Descend one function: its code, calls, tail calls, conflicts, unresolved
        jumps. The rule of opposite conditions holds on a straight line only: where
        it ended a block across an address that something also jumps to (a join, met
        with other flags), the descent is done again without it there."""
        joins = set()
        while True:
            f.__init__(f.entry, f.thumb)
            bad = self._walk_once(f, commit, joins)
            if bad <= joins:
                return f
            joins |= bad

    def _walk_once(self, f, commit, joins):
        """One descent; returns the joins the rule of opposite conditions was applied across."""
        thumb = f.thumb
        mem = self.mem
        todo = [(f.entry, {})]
        seen = set()
        targets, ruled = {f.entry}, []
        own = set()         # the bytes this descent loads as literals
        while todo:
            pc, regs = todo.pop()
            regs = dict(regs)
            ended = None    # condition of the last conditional transfer, while the flags hold
            since = []      # the addresses passed since it
            while True:
                if pc in seen:
                    break
                if pc != f.entry and self.entries.get(pc) == thumb:
                    f.tails.add((pc, thumb))
                    break
                if mem.crosses(f.entry, pc):
                    f.tails.add((pc, thumb))    # into an overlay: an entry there only if it walks clean
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
                    own.update(range(ins.target, ins.target + lit))
                    if commit:
                        self.data.update(range(ins.target, ins.target + lit))
                    if lit == 4:
                        v = mem.u32(ins.target)
                        if v is not None:
                            self.literal_loaded.add(v)
                src = regs.get(ins.rm) if ins.rm is not None else None
                src_alt = regs.get(("alt", ins.rm)) if ins.rm is not None else None
                if ins.rm == 15 and ins.op in ("bx", "blx"):
                    src = ins.pc + (4 if thumb else 8)      # bx pc: Thumb's veneer into ARM
                lr_before = regs.get(14)
                self._consts_step(regs, ins)
                if ins.rd == 14 and not thumb and ins.cond == 14 and 14 in regs and (
                        ins.op == "mov" and ins.rm == 15 or ins.op in ("add", "sub") and ins.rn == 15):
                    todo.append((regs[14], {}))     # a return address of its own: code that
                    targets.add(regs[14])           # runs when a local subroutine returns
                nxt = pc + ins.size
                op, cond = ins.op, ins.cond
                # a conditional transfer falls through, unless the one before it had
                # the opposite condition on the same flags (bxpl ...; popmi {..., pc})
                if pc in joins:     # met by another path too: its flags are not this path's
                    ended = None
                if ended is not None:
                    since.append(pc)
                go_on = cond != 14 and not (ended is not None and cond == ended ^ 1)
                if cond != 14 and not go_on:
                    ruled.append(list(since))
                if ins.s or op in arm.COMPARES:
                    ended, since = None, []
                if op in ("bl", "blx") and ins.target is not None:
                    f.calls.add((ins.target, ins.exchange == "thumb" if ins.exchange else thumb))
                    pc = nxt
                    continue
                if op == "blx":             # through a register
                    if src is not None:
                        f.calls.add((src & ~1, bool(src & 1)))
                    else:
                        f.unresolved.append((pc, "call " + ins.text))
                    f.sites[pc] = ("call", src, nxt | thumb)
                    pc = nxt
                    continue
                if op == "b":
                    targets.add(ins.target)
                    if not go_on:
                        if self.entries.get(ins.target) == thumb and ins.target != f.entry:
                            f.tails.add((ins.target, thumb))
                            break
                        pc = ins.target
                        continue
                    todo.append((ins.target, regs))
                    ended, since = cond, []
                    pc = nxt
                    continue
                if not ins.writes_pc:
                    pc = nxt
                    continue
                # everything else that writes pc
                if op == "bx" and ins.rm == 14 or op == "mov" and ins.rm == 14 and ins.imm is None \
                        or op == "ldm" or op == "ldr" and ins.rn == 13 and ins.writeback:
                    f.sites[pc] = ("return", None, None)
                    if go_on:                   # a conditional return
                        ended, since = cond, []
                        pc = nxt
                        continue
                    break
                sw = self._switch(f, ins, regs, commit)
                if sw is not None:
                    lr = self._lr_set(pc, thumb)
                    f.sites[pc] = ("switch", None, None if lr is None else lr | thumb)
                    todo.extend((t, {}) for t in sw)
                    targets.update(sw)
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
                f.sites[pc] = (kind.strip(), target, lr)
                if target is not None:
                    (f.calls if lr is not None else f.tails).add((target & ~1, bool(target & 1)))
                else:
                    f.unresolved.append((pc, kind + ins.text))
                    if op in ("bx", "mov") and ins.imm is None and src_alt:   # one of several: each an entry
                        for t in src_alt:
                            (f.calls if lr is not None else f.tails).add((t & ~1, op == "bx" and bool(t & 1)))
                if lr is not None:
                    if lr & ~1 != nxt:
                        todo.append((lr & ~1, {}))
                        targets.add(lr & ~1)
                        if not go_on:
                            break
                        ended, since = cond, []
                    pc = nxt
                    continue
                if go_on:
                    ended, since = cond, []
                    pc = nxt
                    continue
                break
        f.size = sum(mem.insn(a, thumb).size for a in f.code)
        if not commit:
            # a trial descent marks no data, so its own literals are checked here:
            # code that loads its own instructions as literals is data read as code
            # (a string whose address is odd, taken for a Thumb entry)
            hit = [c for c in f.code if any(b in own for b in range(c, c + (2 if thumb else 4)))]
            if hit:
                f.conflicts.append((min(hit), "runs into its own literal"))
        return {a for path in ruled for a in path if a in targets}

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
        if thumb or ins.rm is None or ins.rs is not None or ins.imm is not None:
            return None
        # CodeWarrior: [cmp rI, #N] addls pc, pc, rI, lsl #2 ; b default ; N+1 entries
        if ins.op == "add" and ins.rd == 15 and ins.rn == 15 and ins.shift == "lsl" and ins.shift_imm == 2:
            first = ins.pc + 8
            n = self._bound(ins.pc, ins.rm, thumb, signed=ins.cond == 10)
            if n is not None:
                targets = [first + 4 * k for k in range(n + 1)]
            elif mem.insn(first, False) is not None and mem.insn(first, False).op == "b":
                targets = []            # no bound found: the table's branches
                while len(targets) < 256:
                    t = mem.insn(first + 4 * len(targets), False)
                    if t is None or t.op != "b" or t.cond != 14:
                        break
                    targets.append(t.pc)
            elif ins.cond == 14:
                targets = self._run(first)  # a jump into unrolled code: any of its instructions
            else:
                return None
            if ins.cond != 14:
                targets = [ins.pc + 4] + targets
            self.switches[ins.pc] = targets
            return targets
        # a table of offsets from its own address: ldr rX, [rT, rI{, lsl #2}],
        # add pc, rT, rX; or add rY, rT, rX, ..., mov pc, rY (bx rY), the
        # registers followed through the straight line (_consts_step)
        if ins.shift is not None:
            return None
        table = None
        if ins.op == "add":
            for a, b in ((ins.rn, ins.rm), (ins.rm, ins.rn)):
                if a in regs and regs.get(("w", b)) == regs[a]:
                    table = regs[a]
        elif ins.op in ("mov", "bx"):
            table = regs.get(("o", ins.rm))
        if table is None:
            return None
        targets = self._offset_table(table)
        if not targets:
            return None
        if commit:
            self.data.update(range(table, table + 4 * len(targets)))
        lr = self._lr_set(ins.pc, False)
        if lr is not None:
            targets = targets + [lr]
        self.switches[ins.pc] = targets
        return targets

    def _offset_table(self, table):
        """Targets of a table of words that are offsets from the table to ARM
        code, before or after it: read while each one lands on an instruction,
        outside the table, and before the first target that follows it. An
        offset of 0 is a hole (a code with no case), skipped while a target
        follows (four in a row end the table)."""
        mem = self.mem
        targets, limit, holes = [], None, 0
        for k in range(1024):
            at = table + 4 * k
            if limit is not None and at >= limit:
                break
            off = mem.u32(at)
            if off is None:
                break
            if off == 0 and targets:
                holes += 1
                if holes == 4:
                    break
                continue
            holes = 0
            t = (table + off) & 0xFFFFFFFF
            if not mem.inside(t, 4) or t & 3 or table <= t < at + 4:
                break
            ti = mem.insn(t, False)
            if ti is None or ti.op == "undefined":
                break
            targets.append(t)
            if t > table:
                limit = t if limit is None else min(limit, t)
        return targets

    def _run(self, first, limit=1024):
        """The ARM instructions from `first` to the first that does not run on
        (a transfer with no condition), that one included."""
        out = []
        a = first
        while len(out) < limit:
            ins = self.mem.insn(a, False)
            if ins is None or ins.op == "undefined" or any(b in self.data for b in range(a, a + 4)):
                break
            out.append(a)
            if ins.writes_pc and ins.cond == 14 and not ins.is_call:
                break
            a += 4
        return out

    def _bound(self, at, reg, thumb, signed=False):
        """N from the `cmp reg, #N` that guards a switch, walking back through
        a straight line (conditional branches out of it included); through
        `subs reg, rX, #k` after a `cmp rX, #N` (a range of cases from k): N - k."""
        step = 2 if thumb else 4
        k = 0
        for back in range(1, 10):
            p = self.mem.insn(at - step * back, thumb)
            if p is None:
                return None
            if p.op == "cmp" and p.rn == reg and p.imm is not None:
                if signed and p.imm == 0 and not k:
                    continue            # `addge` after `cmp rI, #0`: the lower bound; the upper is further back
                return p.imm - k if p.imm >= k else None
            if p.op == "sub" and p.s and p.rd == reg and p.imm is not None and p.rn != reg \
                    and p.cond == 14 and p.shift is None:
                reg, k = p.rn, k + p.imm
                continue
            if reg in _clobbers(p) or (p.writes_pc and (p.cond == 14 or p.is_call)):
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
                if self.mem.crosses(a, c):
                    self.crossing.add((c, ct))
                elif self.mem.inside(c) and c not in self.entries:
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
            new = self._pointer_seeds() | self._prologue_seeds() | self._crossing_seeds()
            new = {(a, t) for a, t in new if a not in self.entries}
            if not new or len(self.entries) == last:
                break
            last = len(self.entries)
            for a, t in new:
                self.entries[a] = t
        self.returns_twice = self._returns_twice()
        self.landings = self._landings()
        return self

    def _returns_twice(self):
        """Functions that store an address of their own code to memory: a
        context save (NitroSDK's OS_SaveContext: `add r0, pc, #k; str r0,
        [r1, #0x3c]`, then `mov r0, #0; bx lr`), which returns once now and
        again when the context is loaded, at the stored address."""
        out = set()
        for e, f in self.funcs.items():
            if f.thumb or not f.code:
                continue
            hi = max(f.code) + 0x20
            for a in sorted(f.code):
                ins = self.mem.insn(a, False)
                if ins.op not in ("add", "sub") or ins.rn != 15 or ins.target is None or ins.rd in (14, 15) \
                        or not e <= ins.target < hi:
                    continue
                for b in range(a + 4, a + 20, 4):
                    j = self.mem.insn(b, False)
                    if b not in f.code or j is None:
                        break
                    if j.op == "str" and j.rd == ins.rd:
                        out.add(e)
                        break
                    if ins.rd in _clobbers(j) or j.writes_pc:
                        break
        return out

    def _landings(self):
        """{caller entry: return addresses of its calls to a function that
        returns twice}: where control comes back without a call when the saved
        context is loaded (recomp.emit makes them labels to unwind to)."""
        out = {}
        for e, f in self.funcs.items():
            for a in f.code:
                ins = self.mem.insn(a, f.thumb)
                if ins.op in ("bl", "blx") and ins.target is not None and ins.target in self.returns_twice:
                    out.setdefault(e, set()).add(a + ins.size)
        return out

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

    def _crossing_seeds(self):
        """Calls and jumps into an overlay from outside it: an entry when its
        descent there is clean (no condition first, no data, no undefined
        instruction, nothing already walked); otherwise they are meant for
        another overlay loaded at the same address."""
        out = set()
        for a, thumb in self.crossing:
            if a in self.entries:
                continue
            ins = self.mem.insn(a, thumb)
            if ins is not None and ins.cond == 14 and self._clean_descent(a, thumb, 1):
                out.add((a, thumb))
        return out

    def _veneer(self, a, thumb):
        """A short clean descent that ends in a jump to a known entry: an
        interrupt table's stubs (`ldr r12, =handler; mov r0, #n; bx r12`)."""
        f = Function(a, thumb)
        saved = dict(self.entries)
        self._walk(f, commit=False)
        self.entries = saved
        if f.conflicts or len(f.code) < 2 or any(c in self.code for c in f.code):
            return False
        return any(t in self.entries for t, _ in f.tails)

    def _data_words(self):
        """Word-aligned addresses that are data or unclassified, inside the segments."""
        for s in self.mem.segs:
            for a in range(s.base, s.end - 3, 4):
                if a not in self.code and (a + 2) not in self.code:
                    yield a

    def _pointer_seeds(self):
        cand, inner = {}, {}
        for a in self._data_words():
            v = self.mem.u32(a)
            if v is None:
                continue
            t, thumb = v & ~1, bool(v & 1)
            if not self.mem.inside(t, 2) or (not thumb and t & 3) or t in self.entries:
                continue
            if t in self.code and self.code[t] == (2 if thumb else 4):
                inner.setdefault((t, thumb), []).append(a)
                continue
            if t in self.code or t in self.data:
                continue
            cand.setdefault((t, thumb), []).append(a)
        # a pointer counts as in a table when a neighbour points at a function,
        # one accepted in this pass included: repeat to a fixed point (the
        # descents are remembered, so a pass costs little)
        out, self._accepted, memo = set(), set(), {}

        def once(fn, *args):
            if args not in memo:
                memo[args] = fn(*args)
            return memo[args]

        pending = dict(cand)
        while True:
            added = False
            for (t, thumb), places in list(pending.items()):
                ins = self.mem.insn(t, thumb)
                if ins is None or ins.cond != 14:     # no function opens with a condition
                    del pending[(t, thumb)]
                    continue
                v = t | thumb
                in_table = any(self._pointer_neighbour(p) for p in places)
                lit, bound = v in self.literal_loaded, self._boundary(t, thumb)
                if _is_prologue(ins, self.mem):
                    ok = once(self._clean_descent, t, thumb, 1)
                elif lit + in_table + bound >= 2:
                    ok = once(self._clean_descent, t, thumb, 1)    # two of the three: even `bx lr` alone
                elif lit or in_table or bound:
                    ok = once(self._clean_descent, t, thumb, 4) or in_table and once(self._veneer, t, thumb)
                else:
                    ok = False
                if ok:
                    out.add((t, thumb))
                    self._accepted.add(t)
                    del pending[(t, thumb)]
                    added = True
            if not added:
                break
        # code already walked as part of another function (a thunk that adjusts
        # `this` and branches into the method, both in vtables): an entry too, when
        # a table or a literal points at it and it starts after a function's end
        for (t, thumb), places in inner.items():
            if ((t | thumb) in self.literal_loaded or any(self._pointer_neighbour(p) for p in places)) \
                    and self._boundary(t, thumb):
                out.add((t, thumb))
        return out

    def _boundary(self, a, thumb):
        """a follows data, or the last instruction of code that does not run on;
        or, unclassified, a word that is a return (`bx lr`, `pop {..., pc}`):
        the end of an unreached function (tiny virtual methods sit in a row)."""
        prev = a - (2 if thumb else 4)
        if prev in self.data:
            return True
        pi = self.mem.insn(prev, thumb)
        if prev in self.code:
            return pi is not None and pi.writes_pc and pi.cond == 14 and not pi.is_call
        if pi is None or pi.cond != 14:
            return False
        return (pi.op == "bx" and pi.rm == 14) or (pi.op == "ldm" and pi.rn == 13 and bool(pi.reglist >> 15 & 1))

    def _pointer_neighbour(self, p):
        for q in (p - 4, p + 4):
            v = self.mem.u32(q)
            if v is not None and self.mem.inside(v & ~1, 2) and ((v & 1) or not v & 3):
                ins = self.mem.insn(v & ~1, bool(v & 1))
                if ins is not None and (_is_prologue(ins) or (v & ~1) in self.entries
                                        or (v & ~1) in getattr(self, "_accepted", ())
                                        or self._veneer_shape(v & ~1, bool(v & 1))):
                    return True
        return False

    def _veneer_shape(self, a, thumb):
        """`ldr rX, [pc, #k]`, then `bx rX` within the next two instructions."""
        ins = self.mem.insn(a, thumb)
        if ins is None or thumb or ins.op != "ldr" or ins.rn != 15 or ins.cond != 14 or ins.rd in (13, 14, 15):
            return False
        for b in (a + 4, a + 8):
            j = self.mem.insn(b, False)
            if j is None:
                return False
            if j.op == "bx" and j.rm == ins.rd and j.cond == 14:
                return True
            if ins.rd in _clobbers(j) or j.writes_pc:
                return False
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


def segments_from_rom(rom, cpu, overlay=None):
    """The program of one CPU of a cartridge, as segments at their run addresses;
    with one of its overlays loaded, if given (its id)."""
    code = rom.arm9 if cpu == 9 else rom.arm7
    params = rom.params if cpu == 9 else rom.params7
    data = code.plain
    if params is None:
        segs = [Segment(code.ram, data, "main")]
    else:
        end = params.autoload_start - code.ram
        segs = [Segment(code.ram, data[:end], "main")]
        from ..hw import name
        for addr, size, bss, src in params.autoloads:
            blk = data[src - code.ram:src - code.ram + size]
            segs.append(Segment(addr, blk, (name(addr, cpu) or "%08X" % addr).split()[0]))
    if overlay is not None:
        o = overlay_of(rom, cpu, overlay)
        segs.append(Segment(o.ram, overlay_data(rom, o), "ov%d" % o.id, o.id))
    return segs, code.entry


def overlay_of(rom, cpu, oid):
    for o in (rom.overlays9 if cpu == 9 else rom.overlays7):
        if o.id == oid:
            return o
    raise ValueError("no overlay %d on the ARM%d" % (oid, cpu))


def overlay_data(rom, o):
    """An overlay's code as it is loaded (the SDK's backward LZ undone)."""
    from ..rom import blz_decompress
    d = rom.file(o.file_id)
    return blz_decompress(d) if o.compressed else d[:o.ram_size]


def overlay_seeds(rom, cpu, oid, mem):
    """An overlay's static initialisers: the pointers between its sinit start and end."""
    o = overlay_of(rom, cpu, oid)
    out = []
    for a in range(o.sinit_start, o.sinit_end, 4):
        v = mem.u32(a)
        if v and mem.inside(v & ~1, 2):
            out.append(v)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help=".nds (with --cpu) or a flat binary (with --base)")
    ap.add_argument("--cpu", type=int, choices=(7, 9), default=9)
    ap.add_argument("--base", help="flat binary: load address (hex)")
    ap.add_argument("--entry", help="flat binary: entry (hex; default the base)")
    ap.add_argument("--seeds", default="", help="more entries, hex, comma-separated (odd = Thumb)")
    ap.add_argument("--overlay", help="with the program, this overlay loaded (its id), or 'all' one at a time; "
                                      "--out then lists the overlay's functions only (OUT may hold {id})")
    ap.add_argument("--report", action="store_true", help="list conflicts and unresolved jumps")
    ap.add_argument("--out", help="write ENTRY<TAB>SIZE<TAB>A|T for every function")
    a = ap.parse_args(argv)
    with open(a.file, "rb") as fh:
        raw = fh.read()
    rom = None
    if a.base is None:
        from ..rom import Rom
        rom = Rom(raw)
    if a.overlay is None:
        overlays = [None]
    elif a.overlay == "all":
        overlays = [o.id for o in (rom.overlays9 if a.cpu == 9 else rom.overlays7)]
    else:
        overlays = [int(a.overlay, 0)]
    program = {}
    for oid in overlays:
        if rom is not None:
            segs, entry = segments_from_rom(rom, a.cpu, oid)
        else:
            base = int(a.base, 16)
            segs, entry = [Segment(base, raw, "main")], int(a.entry or a.base, 16)
        mem = Memory(segs, arch=5 if a.cpu == 9 else 4)
        seeds = [entry] + [int(x, 16) for x in a.seeds.split(",") if x]
        if oid is not None:
            seeds += overlay_seeds(rom, a.cpu, oid, mem)
            print("overlay %d:" % oid)
        prog = Program(mem, seeds).run()
        text, conf, unres = prog.summary()
        print(text)
        if oid is not None:
            ov = [e for e in prog.funcs if mem.seg(e).overlay == oid]
            print("overlay %d: %d functions (%d Thumb), %d reached from outside it" % (
                oid, len(ov), sum(prog.funcs[e].thumb for e in ov),
                len({c for c, _ in prog.crossing} & set(ov))))
        if a.report:
            for e, (at, why) in conf[:200]:
                print("conflict  %08X in %08X: %s" % (at, e, why))
            for e, (at, what) in unres[:400]:
                print("unresolved %08X in %08X: %s" % (at, e, what))
        if a.out:
            with open(a.out.replace("{id}", str(oid)), "w") as fh:
                for e in sorted(prog.funcs):
                    f = prog.funcs[e]
                    if oid is None or mem.seg(e).overlay == oid:
                        fh.write("%08X\t%d\t%s\n" % (e, f.size, "T" if f.thumb else "A"))
            for e, f in prog.funcs.items():
                if mem.seg(e).overlay is None and e not in program:
                    program[e] = f
    if a.out and overlays != [None] and "{id}" in a.out:
        # the program's own functions, with those only its overlays reach
        with open(a.out.replace("{id}", "main"), "w") as fh:
            for e in sorted(program):
                f = program[e]
                fh.write("%08X\t%d\t%s\n" % (e, f.size, "T" if f.thumb else "A"))
        print("program: %d functions with what the overlays reach" % len(program))


if __name__ == "__main__":
    main()
