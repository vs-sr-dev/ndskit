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
   `mov pc, lr`, `pop`/`ldm` with pc, and Thumb's `pop {rX}; [add sp, #k;]
   bx rX` (lr taken back into a low register so that arguments pushed at
   the entry can be dropped). A Thumb `bl` is a long branch, not a call,
   when its target lies inside the function's own span (`bls ok; bl
   default` before a switch) or when what follows it is a literal pool
   (past one `mov r8, r8`): it is walked as a `b`. An address the function puts in lr
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
   value; a conditional one too (`movne lr, pc; bxne r12`, a hook called
   when it is set). Two conditional transfers in a row with opposite conditions on
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
   takes every instruction of the run it lands in, as does an `add pc, pc,
   rI, lsl #3` (an unrolled copy entered by steps of two instructions).
   Tables of halfword offsets right after the jump: CodeWarrior's Thumb
   switch (`adds rB, rI, rI; add rB, pc; ldrh rB, [rB, #k]; lsls rB, rB,
   #16; asrs rB, rB, #16; add pc, rB`, signed offsets from the jump + 4)
   and a newer SDK ARM7's (`add rB, pc, rI, lsl #1; ldrh rB, [rB, #k];
   add pc, pc, rB`), the bound from the compare before (through `bls ok;
   b default` or a long `bl default`). Tables of offsets from
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
   marks: loaded by the code as a literal, in a table (a neighbouring
   word points at a function, a prologue or a veneer; or the word 8 or 12
   bytes away does, past small words: C++'s pointers to member functions
   are records of the function and one or two adjustments; candidates
   with a boundary and a clean descent count for each other), placed after
   data or a function's end (or after an unclassified word that is a
   return, or such a return's literal pool: C++'s tiny virtual methods sit
   in a row), and, with that boundary, filling a gap (a few instructions
   to a return, then known code or a prologue) -- then even a lone `bx lr`
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
   An entry guessed this way whose descent meets data once everything is
   known is refused too, and the discovery done again without it: a
   pointer or a call meant for another overlay over the same addresses.
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
Known gaps: overlays that the game compresses itself (the overlay
table's flag clear, the file in a format of the game's) are read as they
are stored; code a game decrypts at run time (an overlay whose static
initialisers decrypt it, an anti-piracy library) is read encrypted.

Checked with Thumb switches and halfword tables (session 1 of a 2010
port, Thumb in 28% of its ARM9): on that cartridge 163 switches where
there were 118, its ARM7's 39 where there were 2, 0 conflicts in its
program, ARM7 and 19 of its 20 overlays (the twentieth encrypted);
CodeWarrior's Thumb switches on the 2008 cartridge too (its overlays
1 143 and 628 functions more). On the two ports' cartridges nothing
changed.

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
        self.foreign = set()       # the same, found by another overlay's run (a stricter test)
        self.areas = []            # (lo, hi): other overlays' areas, not loaded in this run
        self.far_bl = set()        # Thumb `bl`s used as long branches: they do not return

    def _elsewhere(self, a):
        """a lies in another overlay's area, its code not loaded here."""
        return not self.mem.inside(a) and any(lo <= a < hi for lo, hi in self.areas)

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
        with other flags), the descent is done again without it there. A Thumb `bl`
        whose fall-through runs into data (past one `mov r8, r8`) is a long branch
        that does not come back: the descent is done again with it as one."""
        joins = set()
        while True:
            f.__init__(f.entry, f.thumb)
            far = len(self.far_bl)
            bad = self._walk_once(f, commit, joins)
            if f.thumb and f.code:
                # a bl into the middle of its own function is a long branch too
                # (`bls ok; bl default` before a switch, the default being the epilogue)
                lo, hi = min(f.code), max(f.code)
                for a in f.code:
                    ins = self.mem.insn(a, True)
                    if ins.op == "bl" and lo < ins.target <= hi:
                        self.far_bl.add(a)
            if bad <= joins and len(self.far_bl) == far:
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
            after_bl = None     # a Thumb bl whose fall-through this is (past one `mov r8, r8`)
            while True:
                if pc in seen:
                    break
                if after_bl is not None and self._is_data_after_bl(pc, own):
                    self.far_bl.add(after_bl)
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
                if after_bl is not None and not (op == "mov" and ins.rd == 8 and ins.rm == 8 and pc == after_bl + 4):
                    after_bl = None
                if op in ("bl", "blx") and ins.target is not None:
                    to = (ins.target, ins.exchange == "thumb" if ins.exchange else thumb)
                    if pc in self.far_bl:       # a long branch: as an unconditional b
                        f.sites[pc] = ("jump", ins.target | thumb, None)
                        targets.add(ins.target)
                        if self.entries.get(ins.target) == thumb and ins.target != f.entry:
                            f.tails.add(to)
                            break
                        pc = ins.target
                        continue
                    f.calls.add(to)
                    if thumb and op == "bl":
                        after_bl = pc
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
                        or op == "ldm" or op == "ldr" and ins.rn == 13 and ins.writeback \
                        or self._popped_return(ins):
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
                lr = self._lr_set(pc, thumb, cond)
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

    def _popped_return(self, ins):
        """Thumb's return through a register: `pop {rX}; [add sp, #k;] bx rX`, lr
        taken back into rX so that the arguments pushed at the entry can be
        dropped after it (`push {r0-r3}; push {r3, lr}` ... `pop {r3}; pop {r3};
        add sp, #16; bx r3`)."""
        if not ins.thumb or ins.op != "bx" or ins.cond != 14 or ins.rm in (13, 14, 15):
            return False
        p = self.mem.insn(ins.pc - 2, True)
        if p is not None and p.op == "add" and p.rd == 13 and p.rn == 13 and p.imm is not None:
            p = self.mem.insn(p.pc - 2, True)
        return p is not None and p.op == "ldm" and p.rn == 13 and p.writeback and p.reglist == 1 << ins.rm

    def _is_data_after_bl(self, pc, own):
        """pc, where a Thumb bl would come back, is data: a literal pool."""
        return any(b in self.data or b in own for b in (pc, pc + 1))

    def _lr_set(self, pc, thumb, cond=14):
        """If the instruction before pc set lr to a return address (mov lr, pc /
        add lr, pc, #n), with no condition or the transfer's own (`movne lr, pc;
        bxne r12`), that address; else None."""
        p = self.mem.insn(pc - (2 if thumb else 4), thumb)
        if p is None or p.rd != 14 or p.cond not in (14, cond):
            return None
        if p.op == "mov" and p.rm == 15 and p.imm is None and p.shift_imm == 0:
            return p.pc + 8
        if p.op in ("add", "sub") and p.rn == 15 and p.target is not None:
            return p.target
        return None

    def _switch(self, f, ins, regs, commit):
        """Targets of a recognised computed jump, or None."""
        mem, thumb = self.mem, f.thumb
        half = self._half_table(ins, commit)
        if half is not None:
            return half
        if thumb or ins.rm is None or ins.rs is not None or ins.imm is not None:
            return None
        # a jump into unrolled code by steps of two or more instructions
        # (`rsb r3, r2, #8; add pc, pc, r3, lsl #3`, a copy of 0-8 bytes): any of its instructions
        if ins.op == "add" and ins.rd == 15 and ins.rn == 15 and ins.shift == "lsl" and ins.shift_imm in (3, 4) \
                and ins.cond == 14:
            targets = self._run(ins.pc + 8)
            if not targets:
                return None
            self.switches[ins.pc] = targets
            return targets
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

    def _half_table(self, ins, commit):
        """Targets of a switch through a table of halfword offsets right after
        the jump, or None. CodeWarrior's Thumb switch: [cmp rI, #N; bhi
        default] adds rI, rI, rI; add rI, pc; ldrh rI, [rI, #k]; lsls rI, rI,
        #16; asrs rI, rI, #16; add pc, rI (signed offsets from the jump + 4);
        a newer SDK's ARM code: [cmp rI, #N; bhi default] add rT, pc, rI, lsl
        #1; ldrh rT, [rT, #k]; add pc, pc, rT (offsets from the jump + 8). The
        table holds N + 1 entries; its bytes are data."""
        if ins.op != "add" or ins.rd != 15 or ins.rn != 15 or ins.rm is None or ins.shift is not None \
                or ins.cond != 14:
            return None
        mem, thumb = self.mem, ins.thumb
        step = 2 if thumb else 4
        r = ins.rm
        p = mem.insn(ins.pc - step, thumb)
        signed = False
        if thumb and p is not None and p.op == "mov" and p.shift == "asr" and p.shift_imm == 16 \
                and p.rd == r and p.rm == r:
            q = mem.insn(p.pc - step, True)
            if q is None or not (q.op == "mov" and q.shift == "lsl" and q.shift_imm == 16 and q.rd == r and q.rm == r):
                return None
            signed, p = True, mem.insn(q.pc - step, True)
        if p is None or p.op != "ldrh" or p.rd != r or p.rm is not None or not p.pre or not p.add \
                or p.writeback or p.imm is None or p.cond != 14:
            return None
        rb, k = p.rn, p.imm
        a = mem.insn(p.pc - step, thumb)
        if a is None or a.op != "add" or a.rd != rb or a.cond != 14:
            return None
        if thumb:                               # add rB, pc; before it rB = 2 x rI
            if a.rn != rb or a.rm != 15 or a.shift is not None:
                return None
            pcv = a.pc + 4
            d = mem.insn(a.pc - 2, True)
            if d is not None and d.op == "add" and d.rd == rb and d.rm is not None and d.rn == d.rm \
                    and d.shift is None:
                index = d.rn
            elif d is not None and d.op == "mov" and d.rd == rb and d.shift == "lsl" and d.shift_imm == 1:
                index = d.rm
            else:
                return None
            guard = d.pc
        else:                                   # add rB, pc, rI, lsl #1
            if a.rn != 15 or a.rm is None or a.shift != "lsl" or a.shift_imm != 1:
                return None
            pcv, index, guard = a.pc + 8, a.rm, a.pc
        n = self._bound(guard, index, thumb)
        if n is None or n > 1024:
            return None
        table, base = pcv + k, ins.pc + (4 if thumb else 8)
        targets = []
        for i in range(n + 1):
            h = mem.u16(table + 2 * i)
            if h is None:
                return None
            t = base + (h - 0x10000 if signed and h & 0x8000 else h)
            if t & 1 or (not thumb and t & 3) or not mem.inside(t, step) or table <= t < table + 2 * (n + 1):
                return None
            targets.append(t)
        if commit:
            self.data.update(range(table, table + 2 * (n + 1)))
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
        k = 0
        a = at
        for back in range(1, 10):
            p = self._prev(a, thumb)
            if p is None:
                return None
            a = p.pc
            if p.op == "cmp" and p.rn == reg and p.imm is not None:
                if signed and p.imm == 0 and not k:
                    continue            # `addge` after `cmp rI, #0`: the lower bound; the upper is further back
                return p.imm - k if p.imm >= k else None
            if p.op == "sub" and p.s and p.rd == reg and p.imm is not None and p.rn != reg \
                    and p.cond == 14 and p.shift is None:
                reg, k = p.rn, k + p.imm
                continue
            if p.op in ("b", "bl") and p.cond == 14:    # Thumb: `bls ok; b default` (or a long `bl default`)
                q = self._prev(p.pc, thumb)
                if q is not None and q.op == "b" and q.cond != 14 and q.target == p.pc + p.size:
                    continue
            if reg in _clobbers(p) or (p.writes_pc and (p.cond == 14 or p.is_call)):
                return None
        return None

    def _prev(self, a, thumb):
        """The instruction before a: in Thumb a `bl` (two halfwords) when one ends there."""
        if thumb:
            p = self.mem.insn(a - 4, True)
            if p is not None and p.size == 4:
                return p
            return self.mem.insn(a - 2, True)
        return self.mem.insn(a - 4, False)

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
        """Discover to a fixed point. An entry guessed in a round (from a pointer
        in data, a prologue, a call into an overlay from outside it) whose descent
        meets data once the rest is known was not one: most often another
        overlay's function, loaded at the same addresses or at another base over
        them, reached through a pointer or a call meant for it. It is refused and
        the discovery done again without it."""
        self.refused = set()
        for _ in range(4):
            self.guessed = set()
            self._rounds(max_rounds)
            bad = {a for a in self.guessed if a in self.funcs and self.funcs[a].conflicts}
            if not bad:
                break
            self.refused |= bad
            self.entries, self.crossing, self.switches = {}, set(), {}
            self.literal_loaded, self.funcs, self.code, self.data = set(), {}, {}, set()
        self.returns_twice = self._returns_twice()
        self.landings = self._landings()
        return self

    def _rounds(self, max_rounds):
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
            new = {(a, t) for a, t in new if a not in self.entries and a not in self.refused}
            self.guessed |= {a for a, _ in new}
            if not new or len(self.entries) == last:
                break
            last = len(self.entries)
            for a, t in new:
                self.entries[a] = t

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
        for a, thumb in self.crossing | self.foreign:
            if a in self.entries or a in self.refused:
                continue
            ins = self.mem.insn(a, thumb)
            if ins is None or ins.cond != 14:
                continue
            if (a, thumb) not in self.crossing and not (_is_prologue(ins, self.mem) or self._boundary(a, thumb)):
                continue        # from another overlay's run: it must also start where a function would
            if self._clean_descent(a, thumb, 1):
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
        return any(t in self.entries or self._elsewhere(t) for t, _ in f.tails)

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
        # candidates that start where a function would (a boundary) and descend
        # clean: as a table's neighbour each counts for the others, so a table of
        # tiny methods none of which is known yet still stands
        self._bounded = set()
        for (t, thumb) in cand:
            ins = self.mem.insn(t, thumb)
            if ins is not None and ins.cond == 14 and self._boundary(t, thumb) \
                    and once(self._clean_descent, t, thumb, 1):
                self._bounded.add(t)
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
                gap = bound and self._fills_gap(t, thumb)
                if _is_prologue(ins, self.mem):
                    ok = once(self._clean_descent, t, thumb, 1)
                elif lit + in_table + bound + gap >= 2:
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
        or, unclassified, a word that is a return (`bx lr`, `pop {..., pc}`),
        or the literal pool (a few words, loaded by the code just before) of any
        unconditional transfer that is not a call (`ldr r0, [pc]; bx lr; .word k`,
        a veneer's `ldr r12, [pc]; bx r12; .word f`):
        the end of an unreached function (tiny virtual methods sit in a row)."""
        prev = a - (2 if thumb else 4)
        if prev in self.data:
            return True
        pi = self.mem.insn(prev, thumb)
        if prev in self.code:
            return pi is not None and pi.writes_pc and pi.cond == 14 and not pi.is_call
        if self._ret(pi):
            return True
        return not thumb and self._after_pool(a)

    def _after_pool(self, a):
        """a follows a literal pool of a few words (each one loaded by the code just
        before it) that follows an unconditional transfer: a function's end."""
        for n in (1, 2, 3, 4):
            pool = a - 4 * n
            ri = self.mem.insn(pool - 4, False)
            if pool - 4 in self.code or not self._ends(ri):
                continue
            loaded = set()
            for b in range(pool - 64, pool - 4, 4):
                li = self.mem.insn(b, False)
                if li is not None and li.literal_size == 4 and li.target is not None:
                    loaded.add(li.target)
            if all(w in loaded for w in range(pool, a, 4)):
                return True
        return False

    def _fills_gap(self, a, thumb):
        """A few straight-line instructions from a that end in a return, right
        after which (past a literal pool of a word or two) known code or a
        prologue begins: a tiny function that exactly fills the gap between two
        others (`mvn r0, #1; bx lr` among a class's methods)."""
        step = 2 if thumb else 4
        b = a
        for _ in range(4):
            ins = self.mem.insn(b, thumb)
            if ins is None or ins.cond != 14 or b in self.code or b in self.data:
                return False
            if ins.writes_pc:
                if not self._ret(ins):
                    return False
                for nxt in (b + step, b + step + 4, b + step + 8):
                    if nxt in self.code or nxt in self.entries:
                        return True
                    ni = self.mem.insn(nxt, thumb)
                    if ni is not None and _is_prologue(ni):
                        return True
                return False
            b += step
        return False

    @classmethod
    def _ends(cls, pi):
        """An instruction after which a function's code does not run on: a return,
        or any unconditional transfer that is not a call (a veneer's `bx r12`)."""
        if cls._ret(pi):
            return True
        return pi is not None and pi.cond == 14 and not pi.is_call and (pi.writes_pc or pi.op == "b")

    @staticmethod
    def _ret(pi):
        if pi is None or pi.cond != 14:
            return False
        return (pi.op == "bx" and pi.rm == 14) or (pi.op == "ldm" and pi.rn == 13 and bool(pi.reglist >> 15 & 1))

    def _pointer_neighbour(self, p):
        near = [p - 4, p + 4]
        # C++ pointers to member functions sit in tables of records of two or
        # three words, the function and small adjustments (`this`, a vtable
        # offset): the neighbour is 8 or 12 bytes away past small words
        def small(a):
            m = self.mem.u32(a)
            return m is not None and (m < 0x1000 or m > 0xFFFFF000)
        for step in (8, 12):
            for sign in (-1, 1):
                if all(small(p + sign * k) for k in range(4, step, 4)):
                    near.append(p + sign * step)
        for q in near:
            v = self.mem.u32(q)
            if v is not None and ((v & 1) or not v & 3) and self._elsewhere(v & ~1):
                return True         # a method inherited from another overlay's class (a vtable)
            if v is not None and self.mem.inside(v & ~1, 2) and ((v & 1) or not v & 3):
                ins = self.mem.insn(v & ~1, bool(v & 1))
                if ins is not None and (_is_prologue(ins) or (v & ~1) in self.entries
                                        or (v & ~1) in getattr(self, "_accepted", ())
                                        or (v & ~1) in getattr(self, "_bounded", ())
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
                if prev in self.code:
                    pi = self.mem.insn(prev, False)
                    if pi is None or not (pi.writes_pc and pi.cond == 14 and not pi.is_call):
                        continue
                elif prev not in self.data and not self._after_pool(a):
                    continue                # after data, the end of code, or an end's literal pool
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


def overlay_areas(rom, cpu, oid):
    """The areas of the overlays that do not share overlay oid's: code a run with
    oid loaded may reach (an inherited method, a veneer) but cannot see."""
    o = overlay_of(rom, cpu, oid)
    lo, hi = o.ram, o.ram + o.ram_size
    out = []
    for p in (rom.overlays9 if cpu == 9 else rom.overlays7):
        a, b = p.ram, p.ram + p.ram_size
        if b <= lo or a >= hi:
            out.append((a, b))
    return out


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
        prog = Program(mem, seeds)
        if oid is not None:
            prog.areas = overlay_areas(rom, a.cpu, oid)
        prog = prog.run()
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
