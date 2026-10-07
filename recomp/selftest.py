"""The recompiler's self-test: the interpreter records, the recompiled code replays.

    python -m ndskit.recomp.selftest GAME.nds --cpu 9 --out VECTORS.txt [--auto] [--funcs A,B,...]
                                     [--vectors 16] [--seed 1]
    python -m ndskit.recomp.selftest --optest DIR [--cpu 9]

A vectors file (the format is in runtime/selftest.cpp) says which CPU, which
images to load where and which module to activate, then, function by
function, the registers r0-r14 and the CPSR before and after each call as
armemu computed them, and the crc32 of each memory after the function's
vectors. The `selftest` executable of a generated build replays them on the
recompiled code.

Game functions: `--funcs` names them; `--auto` takes every function of the
program that, on a few random register states, returns without touching
anything but its stack (no hardware, no pointer from a random argument):
the arithmetic, fixed-point and table helpers; failing that, every one that
does the same with r0-r3 pointing into random data and writes only there:
the routines on vectors, matrices, strings and structures. A vector that
faults in the interpreter is left out, and its writes are undone.

The instruction test (`--optest`, or `python -m ndskit.recomp --optest`) is
ndskit's own, one program per CPU (the ARMv5 forms only on the ARM9): every
instruction form the decoder knows that does not leave the function, ARM
and Thumb, drawn at random within each form and kept when the decoder
reads it back as that form (memory instructions with their base registers
pointing into a block of random data); random straight-line sequences and
sequences with forward branches, for the flags computed only where read;
and hand-assembled sequences for the control flow (conditional branches,
loops, calls, interworking both ways, veneers, both kinds of switch, calls
through registers, conditional returns, mode switches with their banked
registers, an exception return).
"""
import argparse
import os
import random
import struct
import zlib

from .. import arm
from .. import armemu
from . import discover

SENTINEL = armemu.RETURN_SENTINEL
M32 = 0xFFFFFFFF
EDGE = [0, 1, 2, 31, 32, 33, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF]
STACK_TOP = 0x023F0000            # the tests' stack, in main RAM
SCRATCH = 0x02300000              # random data the pointer arguments point into (mode args)
SCRATCH_SIZE = 0x40000


class JournalMemory(armemu.Memory):
    """armemu's memory with an undo journal, so a vector that faults leaves no
    trace (the recompiled side never runs it)."""

    def __init__(self):
        super().__init__()
        self.journal = []
        self.code = []               # [(lo, hi)]: a store there faults (code is not rewritten)

    def write(self, addr, size, value):
        a = self.fold(addr & M32) if self.fold else addr
        if any(lo <= a < hi for lo, hi in self.code):
            raise armemu.MemoryFault("store into the code at %08X" % addr)
        buf, o = self.find(addr, size)
        if buf is not None:
            self.journal.append((buf, o, bytes(buf[o:o + size])))
        super().write(addr, size, value)

    def undo(self):
        for buf, o, old in reversed(self.journal):
            buf[o:o + len(old)] = old
        self.journal = []


def new_memory(cpu, images):
    """The DS memory of one CPU (armemu.ds_memory), zero, with the images
    (data, base) copied in: what selftest.cpp starts from."""
    mem = JournalMemory()
    ref = armemu.ds_memory(cpu)
    for lo, hi, _ in ref.regions:
        mem.add(lo, hi - lo)
    mem.fold = ref.fold
    for data, base in images:
        mem.load(base, data)
    return mem


def crcs(mem):
    """crc32 of each region, in selftest.cpp's order (main RAM first)."""
    return [zlib.crc32(buf) for _, _, buf in mem.regions]


def run(mem, cpu, key, st, cp15, max_steps):
    c = armemu.CPU(mem, 5 if cpu == 9 else 4)
    c.cp15 = cp15
    c.set_cpsr(st[15], 0xF)
    c.r[:15] = list(st[:15])
    c.call(key, max_steps=max_steps)
    return c.r[:15] + [c.cpsr]


def rand_word(rng):
    k = rng.random()
    if k < 0.3:
        return rng.getrandbits(32)
    if k < 0.5:
        return rng.randint(-16, 16) & M32
    if k < 0.7:
        return rng.choice(EDGE)
    return rng.getrandbits(16)


def rand_state(rng, bases=(), small=(), blob=None, mode=0x1F):
    """16 words: r0-r14 (lr the sentinel) and the CPSR. `bases`: registers that
    point into `blob` (base, size) (sp word-aligned, others at any byte);
    `small`: registers with a small value (offsets, counts, indices)."""
    w = [rand_word(rng) for _ in range(15)]
    for r in small:
        w[r] = rng.randrange(1, 12)
    for r in bases:
        lo, size = blob
        p = lo + 0x1000 + rng.randrange(0, size - 0x2000)
        w[r] = p & ~3 if r == 13 or rng.random() < 0.7 else p
    w[14] = SENTINEL
    return w + [(rng.getrandbits(5) << 27) | mode]


def record(mem, cpu, key, label, states, cp15, max_steps=200_000):
    """The vector lines for one function (faulting states left out)."""
    lines = ["func %08X %s" % (key, label)]
    for st in states:
        mem.journal = []
        try:
            out = run(mem, cpu, key, st, cp15, max_steps)
        except (armemu.MemoryFault, armemu.Undefined, RuntimeError):
            mem.undo()
            continue
        lines.append("v " + " ".join("%08X" % x for x in st + out))
    mem.journal = []
    lines.append("mem " + " ".join("%08X" % x for x in crcs(mem)))
    return lines, len(lines) - 2


def _own(a):
    return STACK_TOP - 0x10000 <= a < STACK_TOP + 0x100 or SCRATCH <= a < SCRATCH + SCRATCH_SIZE


def pure_functions(cpu, images, prog, rng, trials=6, max_steps=20_000):
    """{key: mode} of the functions that return on random states writing only
    their stack and (mode args) what their pointer arguments point at."""
    out = {}
    code = code_ranges(prog)
    for e in sorted(prog.funcs):
        f = prog.funcs[e]
        key = e | int(f.thumb)
        for mode in ("game", "args"):
            mem = new_memory(cpu, images)
            mem.code = code
            mem.log = []
            ok = True
            for _ in range(trials):
                mem.log.clear()
                try:
                    run(mem, cpu, key, game_state(rng, mode), {}, max_steps)
                except (armemu.MemoryFault, armemu.Undefined, RuntimeError):
                    ok = False
                    break
                if any(k == "w" and not _own(a) for k, a, _, _ in mem.log):
                    ok = False
                    break
            if ok:
                out[key] = mode
                break
    return out


def code_ranges(prog):
    """Each segment's code, from its first instruction to its last."""
    out = []
    for s in prog.mem.segs:
        inside = [a for a in prog.code if s.base <= a < s.end]
        if inside:
            out.append((min(inside), max(inside) + 4))
    return out


def game_state(rng, mode):
    w = rand_state(rng)
    w[13] = STACK_TOP
    if mode == "args":
        for i in range(4):
            w[i] = SCRATCH + rng.randrange(0x100, SCRATCH_SIZE - 0x1000, 4)
    return w


def scratch_data(seed=1):
    rng = random.Random(seed * 104729)
    return bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))


# ---- the instruction test -------------------------------------------------------------
OPTEST_BASE = 0x02000000          # the code (module OPTEST9 / OPTEST7)
BLOB = 0x02200000                 # random data the tests read and write
BLOB_SIZE = 0x40000


class Asm:
    """A few lines of ARM or Thumb with labels. Each item is a function of
    (labels, its own address) returning its bytes; sizes are fixed."""

    def __init__(self, base):
        self.base, self.items, self.labels = base, [], {}
        self.pc = base

    def label(self, name):
        self.labels[name] = self.pc

    def raw(self, size, fn):
        self.items.append((self.pc, size, fn))
        self.pc += size

    def word(self, w):
        self.raw(4, lambda L, pc, w=w: struct.pack("<I", (w(L, pc) if callable(w) else w) & M32))

    def half(self, h):
        self.raw(2, lambda L, pc, h=h: struct.pack("<H", (h(L, pc) if callable(h) else h) & 0xFFFF))

    def align(self, n=4, fill=0):
        while self.pc % n:
            self.half(fill)

    def assemble(self):
        out = bytearray()
        for pc, size, fn in self.items:
            b = fn(self.labels, pc)
            assert len(b) == size
            out += b
        return bytes(out)

    # ARM
    def dp(self, op, rd=0, rn=0, imm=None, rm=None, s=False, cond=14, shift=None, amount=0, rs=None):
        opc = arm.DP_OPS.index(op)
        w = cond << 28 | opc << 21 | int(s or op in arm.COMPARES) << 20 | rn << 16 | rd << 12
        if imm is not None:
            for rot in range(16):
                v = ((imm << (2 * rot)) | (imm >> (32 - 2 * rot))) & M32 if rot else imm
                if v < 256:
                    break
            else:
                raise ValueError(imm)
            w |= 1 << 25 | rot << 8 | v
        else:
            t = ("lsl", "lsr", "asr", "ror").index(shift or "lsl")
            w |= t << 5 | rm
            w |= (rs << 8 | 1 << 4) if rs is not None else (amount & 31) << 7
        self.word(w)

    def b(self, to, cond=14, link=False):
        self.word(lambda L, pc: cond << 28 | 0b101 << 25 | int(link) << 24 | ((L[to] - pc - 8) >> 2) & 0xFFFFFF)

    def blx_imm(self, to):
        self.word(lambda L, pc: 0xFA000000 | ((L[to] - pc - 8) >> 1 & 1) << 24 | ((L[to] - pc - 8) >> 2) & 0xFFFFFF)

    def bx(self, rm, cond=14, link=False):
        self.word(cond << 28 | (0x012FFF30 if link else 0x012FFF10) | rm)

    def ldr_lit(self, rd, to, cond=14):
        def enc(L, pc):
            off = L[to] - (pc + 8)
            return cond << 28 | 0x05100000 | (off >= 0) << 23 | 15 << 16 | rd << 12 | abs(off)
        self.word(enc)

    def mem(self, load, rd, rn, off=0, byte=False, pre=True, wb=False, cond=14):
        self.word(cond << 28 | 1 << 26 | pre << 24 | (off >= 0) << 23 | byte << 22 | wb << 21 | load << 20
                  | rn << 16 | rd << 12 | abs(off))

    def ldm(self, load, rn, regs, mode="ia", wb=False, cond=14):
        p, u = {"ia": (0, 1), "ib": (1, 1), "da": (0, 0), "db": (1, 0)}[mode]
        self.word(cond << 28 | 1 << 27 | p << 24 | u << 23 | wb << 21 | load << 20 | rn << 16
                  | sum(1 << r for r in regs))

    def push(self, regs):
        self.ldm(False, 13, regs, "db", True)

    def pop(self, regs):
        self.ldm(True, 13, regs, "ia", True)

    def mrs(self, rd, spsr=False):
        self.word(0xE10F0000 | spsr << 22 | rd << 12)

    def msr(self, mask, rm=None, imm=None, spsr=False):
        if imm is not None:
            self.word(0xE320F000 | spsr << 22 | mask << 16 | imm)
        else:
            self.word(0xE120F000 | spsr << 22 | mask << 16 | rm)

    # Thumb
    def t_movi(self, rd, imm):
        self.half(0x2000 | rd << 8 | imm)

    def t_addi(self, rd, imm):
        self.half(0x3000 | rd << 8 | imm)

    def t_subi(self, rd, imm):
        self.half(0x3800 | rd << 8 | imm)

    def t_cmpi(self, rd, imm):
        self.half(0x2800 | rd << 8 | imm)

    def t_b(self, to, cond=None):
        if cond is None:
            self.half(lambda L, pc: 0xE000 | ((L[to] - pc - 4) >> 1) & 0x7FF)
        else:
            self.half(lambda L, pc: 0xD000 | cond << 8 | ((L[to] - pc - 4) >> 1) & 0xFF)

    def t_bl(self, to, exchange=False):
        """bl (or blx to ARM): the offset from the prefix's pc + 4 (word-aligned for blx)."""
        def off(L, first):
            return L[to] - (((first + 4) & ~3) if exchange else first + 4)
        self.half(lambda L, pc: 0xF000 | (off(L, pc) >> 12) & 0x7FF)
        self.half(lambda L, pc: (0xE800 if exchange else 0xF800) | (off(L, pc - 2) >> 1) & 0x7FF)

    def t_bx(self, rm, link=False):
        self.half((0x4780 if link else 0x4700) | rm << 3)

    def t_push(self, regs, lr=False):
        self.half(0xB400 | lr << 8 | sum(1 << r for r in regs))

    def t_pop(self, regs, pc=False):
        self.half(0xBC00 | pc << 8 | sum(1 << r for r in regs))

    def t_ldr_lit(self, rd, to):
        self.half(lambda L, pc: 0x4800 | rd << 8 | ((L[to] - ((pc + 4) & ~3)) >> 2))

    def t_mov_hi(self, rd, rm):
        self.half(0x4600 | (rd & 8) << 4 | rm << 3 | rd & 7)

    def t_add_hi(self, rd, rm):
        self.half(0x4400 | (rd & 8) << 4 | rm << 3 | rd & 7)


def _safe_arm(ins, arch):
    """A single ARM instruction the test can run alone: it does not leave
    (no write to pc, no SWI, no coprocessor other than CP15), and its result
    is defined."""
    op = ins.op
    if op in ("undefined", "swi", "bkpt", "b", "bl", "blx", "bx", "cdp", "cdp2", "mcrr", "mrrc",
              "mcr2", "mrc2") or op.startswith(("ldc", "stc")) or ins.writes_pc:
        return False
    if op in ("mcr", "mrc"):
        return ins.cp == 15 and ins.rd != 15
    regs = [r for r in (ins.rd, ins.rn, ins.rm, ins.rs, ins.rd2) if r is not None]
    if op in arm.DATA_OPS:
        return not (ins.rs is not None and 15 in regs) and ins.rd != 14
    if op == "msr":
        return not ins.spsr and ins.mask == 8 and ins.rm != 15
    if op == "mrs":
        return ins.rd != 15
    if op in arm.LOADS or op in arm.STORES:
        if ins.rm == 15 or ins.rn == 15:      # literal loads have their own blocks
            return False
        if ins.writeback and (ins.rn == ins.rd or (op in ("ldrd", "strd") and ins.rn == ins.rd + 1)):
            return False
        if op in ("ldrd", "strd") and (ins.rd & 1 or ins.rd == 14):
            return False
        if ins.rm is not None and ins.rm == ins.rn and ins.writeback:
            return False
        if ins.rd == 14 or ins.rn == 14 or (op in ("ldrd", "strd") and ins.rd + 1 == 14):
            return False                        # lr is the way back
        if op in arm.STORES and ins.rd == 15:
            return False
        return True
    if op in ("ldm", "stm"):
        if ins.user or ins.rn in (14, 15) or (op == "ldm" and ins.reglist & 0x4000):
            return False
        if op == "stm" and ins.reglist & 0x8000:
            return False
        return True
    if 15 in regs or 14 in regs:
        return False
    if op in ("mul", "mla") and arch < 5 and ins.rd == ins.rm:
        return False
    if op in ("umull", "umlal", "smull", "smlal") or op.startswith("smlal"):
        if ins.rd == ins.rd2 or (arch < 5 and ins.rm in (ins.rd, ins.rd2)):
            return False
    if op in ("swp", "swpb") and ins.rn in (ins.rd, ins.rm):
        return False
    return True


def _safe_thumb(ins):
    """A single Thumb instruction the test can run alone (see _safe_arm)."""
    op = ins.op
    if op in ("undefined", "swi", "bkpt", "b", "bl", "blx", "bx", "bl_hi", "bl_lo", "blx_lo") or ins.writes_pc:
        return False
    if ins.rd == 14:
        return False
    if op == "stm" and ins.rn != 13 and ins.reglist >> ins.rn & 1 and ins.reglist & ((1 << ins.rn) - 1):
        return False                            # stmia rn!, {.., rn, ..} with rn not the lowest
    return True


def _plain_thumb(ins):
    """A Thumb instruction for the random sequences: no memory, no sp or pc."""
    regs = [r for r in (ins.rd, ins.rn, ins.rm, ins.rs) if r is not None]
    return _safe_thumb(ins) and ins.size == 2 and not _bases(ins) and ins.op not in ("ldm", "stm") \
        and 13 not in regs and 15 not in regs


def _arm_word(rng, k):
    """A random ARM word of class k, condition always (or random 1 time in 5)."""
    cond = 14 if rng.random() < 0.8 else rng.randrange(14)
    if k == 0:                                    # data processing, register or immediate
        w = rng.getrandbits(26) & 0x03FFFFFF
        if not w & 0x02000000 and w & 0x10:
            w &= ~0x80                            # shift by a register: bit 7 clear
    elif k == 1:                                  # single loads and stores
        w = 0x04000000 | rng.getrandbits(26) & 0x03FFFFEF
        if w & 0x02000000 and rng.random() < 0.7:
            w &= ~0xF80                           # small shifts of the offset register
            w |= rng.randrange(4) << 7
        if not w & 0x02000000:
            w &= ~0xF00 if rng.random() < 0.7 else ~0   # offsets under 256 most of the time
    elif k == 2:                                  # ldm/stm
        w = 0x08000000 | rng.getrandbits(25) & 0x01BF0000 | rng.getrandbits(16) & rng.getrandbits(16)
    elif k == 3:                                  # multiplies, swp, halfword/double transfers
        w = rng.getrandbits(28) & 0x01FFFF0F | 0x90 | rng.getrandbits(2) << 5
        if w & 0x60 and not w & 0x00400000:
            w &= ~0xF00                           # register offset: bits 11-8 are zero
        if not w & 0x60:
            w &= ~(rng.getrandbits(1) << 24)
    elif k == 4:                                  # the ARMv5 DSP group, clz
        w = 0x01000000 | rng.getrandbits(24) & 0x006FFFFF
        w = (w & ~0xF0) | rng.choice([0x80, 0xA0, 0xC0, 0xE0, 0x50, 0x10])
        if (w & 0xF0) == 0x10:
            w = (w & ~0x00600000) | 0x00600000 | 0x000F0F00
    elif k == 5:                                  # mrs, msr flags, mcr/mrc p15
        r = rng.random()
        if r < 0.3:
            w = 0x010F0000 | rng.randrange(13) << 12
        elif r < 0.6:
            w = 0x0128F000 | rng.randrange(13) if rng.random() < 0.5 else 0x0328F000 | rng.getrandbits(12)
        else:
            w = 0x0E000F10 | rng.getrandbits(3) << 21 | rng.getrandbits(1) << 20 | rng.getrandbits(4) << 16 \
                | rng.randrange(13) << 12 | rng.getrandbits(3) << 5 | rng.getrandbits(4)
    return cond << 28 | w & 0x0FFFFFFF


def _bases(ins):
    """Registers to point into the blob for a memory instruction."""
    op = ins.op
    if op in arm.LOADS or op in arm.STORES or op in ("swp", "swpb", "ldm", "stm"):
        return [ins.rn] if ins.rn != 15 else []
    return []


def _small(ins):
    if (ins.op in arm.LOADS or ins.op in arm.STORES) and ins.rm is not None:
        return [ins.rm]
    return []


def optest(cpu, seed=1, per_class=None):
    """(code image, [(key, label, bases, small)]) of the instruction test for one CPU."""
    rng = random.Random(seed * 31 + cpu)
    arch = 5 if cpu == 9 else 4
    a = Asm(OPTEST_BASE)
    funcs = []

    def func(label, bases=(), small=(), thumb=False):
        a.align(4)
        if thumb:
            funcs.append((a.pc | 1, label, list(bases), list(small)))
        else:
            funcs.append((a.pc, label, list(bases), list(small)))

    # ---- single ARM instructions, each alone before bx lr
    counts = per_class or {0: 900, 1: 500, 2: 250, 3: 400, 4: 200 if arch >= 5 else 0, 5: 120}
    for k, n in counts.items():
        made = 0
        tries = 0
        while made < n and tries < n * 200:
            tries += 1
            w = _arm_word(rng, k)
            ins = arm.decode_arm(w, a.pc, arch)
            if not _safe_arm(ins, arch):
                continue
            if k == 2 and ins.op not in ("ldm", "stm"):
                continue
            func(ins.text, _bases(ins), _small(ins))
            a.word(w)
            a.bx(14)
            made += 1
    # ---- literal loads: the load, bx lr, then the literal (folded into a constant)
    for op, sh, size in (("ldr", 0, 4), ("ldrb", 0, 1), ("ldrh", 1, 2), ("ldrsb", 2, 1), ("ldrsh", 3, 2)):
        for k in range(6):
            rd = rng.randrange(13)
            o = rng.randrange(0, 4, size)        # which byte (halfword) of the literal it reads
            func("literal %s r%d" % (op, rd))
            if op in ("ldr", "ldrb"):            # ldr{b} rd, [pc, #o]: pc + 8 is the literal
                a.word(0xE59F0000 | (op == "ldrb") << 22 | rd << 12 | o)
            else:                                # ldr{h,sb,sh} rd, [pc, #o]
                a.word(0xE1DF0090 | rd << 12 | sh << 5 | o)
            a.bx(14)
            a.word(rng.getrandbits(32))
    for k in range(6):
        rd = rng.randrange(8)
        func("thumb literal r%d" % rd, thumb=True)
        a.half(0x4800 | rd << 8 | 0)             # ldr rd, [pc, #0]: the word after bx lr
        a.t_bx(14)
        a.word(rng.getrandbits(32))
    # ---- single Thumb instructions
    made = 0
    while made < 900:
        h = rng.getrandbits(16)
        ins = arm.decode_thumb(h, 0, None, arch)
        if not _safe_thumb(ins) or ins.size != 2 or ins.rn == 15:
            continue
        bases = _bases(ins)
        if ins.op in ("ldm", "stm") and ins.rn == 13:
            bases = [13]
        func("thumb " + ins.text, bases, _small(ins), thumb=True)
        a.half(h)
        a.t_bx(14)
        made += 1
    # ---- random straight-line ARM sequences (the flags: computed only where read)
    for i in range(150):
        func("arm sequence %d" % i)
        for _ in range(rng.randint(3, 9)):
            while True:
                w = _arm_word(rng, rng.choice([0, 0, 0, 3]))
                ins = arm.decode_arm(w, a.pc, arch)
                if _safe_arm(ins, arch) and not _bases(ins) and ins.op not in ("mrs", "msr"):
                    break
            a.word(w)
        a.bx(14)
    # ---- random sequences with forward branches (liveness across joins)
    for i in range(150):
        func("arm branches %d" % i)
        n = rng.randint(4, 10)
        lab = "fb%d_" % i
        pending = []
        for j in range(n):
            if rng.random() < 0.3:
                to = lab + str(rng.randint(j + 1, n))
                pending.append(to)
                a.b(to, cond=rng.randrange(15))
            else:
                while True:
                    w = _arm_word(rng, 0)
                    ins = arm.decode_arm(w, a.pc, arch)
                    if _safe_arm(ins, arch) and not _bases(ins):
                        break
                a.word(w)
            a.label(lab + str(j + 1))
        a.bx(14)
    for i in range(120):
        func("thumb sequence %d" % i, thumb=True)
        lab = "tb%d_" % i
        n = rng.randint(3, 9)
        for j in range(n):
            if rng.random() < 0.25:
                a.t_b(lab + str(rng.randint(j + 1, n)), cond=rng.randrange(14))
            else:
                while True:
                    h = rng.getrandbits(16)
                    if _plain_thumb(arm.decode_thumb(h, 0, None, arch)):
                        break
                a.half(h)
            a.label(lab + str(j + 1))
        a.t_bx(14)
    _control(a, func, arch)
    return a.assemble(), funcs


def _control(a, func, arch):
    """Hand-written control flow."""
    # a loop: r1 iterations of r3 += r2
    func("loop subs/bne", small=[1])
    a.label("L1")
    a.dp("add", 3, 3, rm=2)
    a.dp("sub", 1, 1, imm=1, s=True)
    a.b("L1", cond=1)
    a.bx(14)
    # a call, with the stack
    func("subroutine", bases=[13])
    a.label("SUB")
    a.dp("add", 3, 3, rm=1)
    a.dp("eor", 0, 0, rm=3, s=True)
    a.bx(14)
    func("bl and push/pop", bases=[13])
    a.push([4, 14])
    a.dp("mov", 4, 0, rm=1)
    a.b("SUB", link=True)
    a.dp("add", 1, 1, rm=4)
    a.b("SUB", link=True)
    a.pop([4, 15])
    func("conditional call and return", bases=[13])
    a.push([14])
    a.dp("cmp", 0, 0, rm=1)
    a.b("SUB", cond=11, link=True)          # bllt
    a.dp("mov", 2, 0, imm=5, cond=10)
    a.pop([14])
    a.bx(14, cond=0)                         # bxeq lr
    a.dp("add", 2, 2, imm=1)
    a.bx(14)
    # a call through a register: mov lr, pc; bx r12, and blx
    func("call through a register", bases=[13])
    a.push([14])
    a.ldr_lit(12, "LIT_SUB")
    a.dp("mov", 14, 0, rm=15)
    a.bx(12)
    a.ldr_lit(5, "LIT_SUB")
    if arch >= 5:
        a.bx(5, link=True)
    a.pop([15])
    a.label("LIT_SUB")
    a.word(lambda L, pc: L["SUB"])
    # a long-branch veneer: ldr pc, =target as a tail
    func("veneer", bases=[13])
    a.ldr_lit(12, "LIT_SUB2")
    a.bx(12)
    a.label("LIT_SUB2")
    a.word(lambda L, pc: L["SUB"])
    func("ldr pc veneer", bases=[13])
    a.word(lambda L, pc: 0xE51FF004)         # ldr pc, [pc, #-4]
    a.word(lambda L, pc: L["SUB"])
    # CodeWarrior's switch
    func("switch addls", small=[0])
    a.dp("cmp", 0, 0, imm=3)
    a.dp("add", 15, 15, rm=0, shift="lsl", amount=2, cond=9)
    a.b("SWD")
    for i in range(4):
        a.b("SW%d" % i)
    for i in range(4):
        a.label("SW%d" % i)
        a.dp("mov", 2, 0, imm=10 + i)
        a.bx(14)
    a.label("SWD")
    a.dp("mov", 2, 0, imm=99)
    a.bx(14)
    # a table of offsets: add r3, pc, #k; ldr r2, [r3, r0, lsl #2]; add pc, r2, r3
    func("offset table", small=[0])
    a.dp("and", 0, 0, imm=3)
    a.word(lambda L, pc: 0xE28F3000 | (L["OT"] - pc - 8))     # add r3, pc, #(OT - pc - 8)
    a.word(0xE7932100)                       # ldr r2, [r3, r0, lsl #2]
    a.word(0xE082F003)                       # add pc, r2, r3
    a.label("OT")
    for i in range(4):
        a.word(lambda L, pc, i=i: L["OC%d" % i] - L["OT"])
    for i in range(4):
        a.label("OC%d" % i)
        a.dp("add", 1, 1, imm=1 << i)
        a.bx(14)
    # interworking
    a.align(4)
    func("thumb subroutine", thumb=True)
    a.label("TSUB")
    a.half(0x1889)                           # adds r1, r1, r2
    a.t_bx(14)
    if arch >= 5:
        func("blx to thumb", bases=[13])
        a.push([14])
        a.blx_imm("TSUB")
        a.pop([15])
        func("thumb bl and blx to arm", bases=[13], thumb=True)
        a.t_push([], lr=True)
        a.t_bl("TSUB")
        a.t_bl("SUB", exchange=True)
        a.t_pop([], pc=True)
    func("bx to thumb", bases=[13])
    a.push([14])
    a.ldr_lit(3, "LIT_TSUB")
    a.dp("mov", 14, 0, rm=15)
    a.bx(3)
    a.pop([14])
    a.bx(14)
    a.label("LIT_TSUB")
    a.word(lambda L, pc: L["TSUB"] | 1)
    a.align(4)
    func("thumb bx pc to arm", thumb=True)
    a.t_bx(15)
    a.half(0x46C0)                           # nop (mov r8, r8)
    a.dp("add", 0, 0, imm=7)
    a.bx(14)
    func("thumb push/pop return", bases=[13], thumb=True)
    a.t_push([4, 5], lr=True)
    a.t_movi(4, 3)
    a.half(0x1900)                           # adds r0, r0, r4
    if arch >= 5:
        a.t_pop([4, 5], pc=True)
    else:                                    # the ARMv4T's way back: pop, then bx
        a.t_pop([4, 5])
        a.t_pop([3])
        a.t_bx(3)
    func("thumb loop", small=[1], thumb=True)
    a.label("TL")
    a.half(0x18D2)                           # adds r2, r2, r3
    a.t_subi(1, 1)
    a.t_b("TL", cond=1)
    a.t_bx(14)
    func("thumb literal and adr", thumb=True)
    a.t_ldr_lit(0, "TLIT")
    a.half(0xA101)                           # add r1, pc, #4
    a.t_bx(14)
    a.align(4)
    a.label("TLIT")
    a.word(0x12345678)
    # modes: into IRQ mode and back, its banked sp/lr seen; the SPSR
    func("mode switch and banks")
    a.mrs(0)
    a.dp("bic", 1, 0, imm=0x1F)
    a.dp("orr", 1, 1, imm=0x12)
    a.msr(1, rm=1)                           # cpsr_c: IRQ mode
    a.dp("mov", 2, 0, rm=13)
    a.dp("mov", 3, 0, rm=14)
    a.dp("mov", 13, 0, rm=4)
    a.msr(9, rm=5, spsr=True)                # spsr_fc
    a.mrs(6, spsr=True)
    a.msr(1, rm=0)                           # back
    a.dp("mov", 7, 0, rm=13)
    a.bx(14)
    func("fiq bank")
    a.mrs(0)
    a.dp("bic", 1, 0, imm=0x1F)
    a.dp("orr", 1, 1, imm=0x11)
    a.dp("mov", 9, 0, imm=0x55)
    a.msr(1, rm=1)
    a.dp("mov", 2, 0, rm=9)
    a.dp("mov", 8, 0, imm=0x77)
    a.msr(1, rm=0)
    a.dp("mov", 3, 0, rm=8)
    a.bx(14)
    func("user mode stays")
    a.mrs(0)
    a.dp("bic", 1, 0, imm=0x1F)
    a.dp("orr", 1, 1, imm=0x10)
    a.msr(1, rm=1)
    a.msr(1, imm=0x13)                       # ignored in user mode
    a.mrs(2)
    a.bx(14)
    func("exception return")
    a.mrs(0)
    a.dp("bic", 1, 0, imm=0x1F)
    a.dp("orr", 1, 1, imm=0x12)
    a.msr(1, rm=1)                           # IRQ mode
    a.msr(15, rm=0, spsr=True)               # spsr = the caller's cpsr
    a.dp("mvn", 14, 0, imm=0xB)              # lr_irq = 0xFFFFFFF4, the sentinel + 4
    a.dp("sub", 15, 14, imm=4, s=True)       # subs pc, lr, #4


def optest_blob(seed=1):
    rng = random.Random(seed * 7919)
    return bytes(rng.getrandbits(8) for _ in range(BLOB_SIZE))


def write_optest(out_dir, cpu, seed=1, vectors=8):
    """Write optest<cpu>.bin, optest<cpu>_blob.bin and optest<cpu>.txt (the vectors)
    into out_dir; returns (module name, segments, seeds)."""
    os.makedirs(out_dir, exist_ok=True)
    code, funcs = optest(cpu, seed)
    blob = optest_blob(seed)
    name = "OPTEST%d" % cpu
    code_path = os.path.abspath(os.path.join(out_dir, "optest%d.bin" % cpu)).replace("\\", "/")
    blob_path = os.path.abspath(os.path.join(out_dir, "optest%d_blob.bin" % cpu)).replace("\\", "/")
    for path, data in ((code_path, code), (blob_path, blob)):
        with open(path, "wb") as f:
            f.write(data)
    rng = random.Random(seed + cpu)
    mem = new_memory(cpu, [(code, OPTEST_BASE), (blob, BLOB)])
    mem.code = [(OPTEST_BASE, OPTEST_BASE + len(code))]
    cp15 = {}
    lines = ["# ndskit instruction test: ARM%d, %d functions" % (cpu, len(funcs)),
             "cpu %d" % cpu,
             "image %08X %s" % (OPTEST_BASE, code_path),
             "image %08X %s" % (BLOB, blob_path),
             "module %s" % name]
    empty = []
    for key, label, bases, small in funcs:
        states = [rand_state(rng, bases, small, (BLOB, BLOB_SIZE)) for _ in range(vectors)]
        ls, k = record(mem, cpu, key, label, states, cp15, max_steps=10_000)
        if not k:
            empty.append(label)
        lines += ls
    with open(os.path.join(out_dir, "optest%d.txt" % cpu), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if empty:
        print("%s: %d functions have no vector that runs: %s" % (name, len(empty), ", ".join(empty[:8])))
    seg = discover.Segment(OPTEST_BASE, code, "main")
    return name, [seg], [k for k, _, _, _ in funcs]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rom", nargs="?")
    ap.add_argument("--optest", metavar="DIR", help="write the instruction test into DIR")
    ap.add_argument("--cpu", type=int, choices=(7, 9), default=9)
    ap.add_argument("--funcs", default="", help="entries to test (hex, comma-separated; odd = Thumb)")
    ap.add_argument("--auto", action="store_true", help="also every function that runs alone")
    ap.add_argument("--vectors", type=int, default=16)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    if a.optest:
        name, segs, seeds = write_optest(a.optest, a.cpu, a.seed)
        print("%s: %d functions at %08X" % (name, len(seeds), segs[0].base))
        return
    from ..rom import Rom
    with open(a.rom, "rb") as fh:
        rom = Rom(fh.read())
    segs, entry = discover.segments_from_rom(rom, a.cpu)
    mem0 = discover.Memory(segs, arch=5 if a.cpu == 9 else 4)
    prog = discover.Program(mem0, [entry]).run()
    out_dir = os.path.dirname(os.path.abspath(a.out))
    images = [(s.data, s.base) for s in segs]
    data = scratch_data(a.seed)
    images.append((data, SCRATCH))
    name = "ARM%d" % a.cpu
    lines = ["# ndskit self-test: %s" % name, "cpu %d" % a.cpu]
    for i, s in enumerate(segs):
        path = os.path.join(out_dir, "%s_seg%d.bin" % (name.lower(), i)).replace("\\", "/")
        with open(path, "wb") as f:
            f.write(s.data)
        lines.append("image %08X %s" % (s.base, path))
    scratch = os.path.join(out_dir, "%s_scratch.bin" % name.lower()).replace("\\", "/")
    with open(scratch, "wb") as f:
        f.write(data)
    lines.append("image %08X %s" % (SCRATCH, scratch))
    lines.append("module %s" % name)
    rng = random.Random(a.seed)
    funcs = {int(x, 16): "game" for x in a.funcs.split(",") if x}
    if a.auto:
        found = pure_functions(a.cpu, images, prog, rng)
        n_args = sum(1 for m in found.values() if m == "args")
        print("%s: %d of %d functions run on their stack alone, %d more with pointer arguments"
              % (name, len(found) - n_args, len(prog.funcs), n_args))
        for k, mode in found.items():
            funcs.setdefault(k, mode)
    mem = new_memory(a.cpu, images)
    mem.code = code_ranges(prog)
    total = 0
    cp15 = {}
    for key, mode in funcs.items():
        if key & ~1 not in prog.funcs:
            print("  %08X is not an entry of %s: skipped" % (key, name))
            continue
        ls, k = record(mem, a.cpu, key, "%s_%08X (%s)" % (name.lower(), key, mode),
                       [game_state(rng, mode) for _ in range(a.vectors)], cp15)
        lines += ls
        total += k
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d functions, %d vectors -> %s" % (name, len(funcs), total, a.out))


if __name__ == "__main__":
    main()
