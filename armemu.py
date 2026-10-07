"""An ARM interpreter (ARMv5TE and ARMv4T, ARM and Thumb), for running guest functions.

    python -m ndskit.armemu GAME.nds --cpu 9 --call 0201234C [--thumb] [--regs r0=7,r1=100]
    python -m ndskit.armemu FILE.bin --base 02000000 --call 02000C00 ...

It is the reference the recompiler is checked against (as saturnkit's
sh2emu is saturnkit's), and a way to ask the game's own code a question
(what does this helper compute?) without an emulator. It models the CPU
only: the sixteen registers and the processor modes that bank r13/r14 (and
r8-r12 in FIQ), the CPSR (N Z C V, Q on the ARMv5, I F T and the mode) and
each mode's SPSR, the PC read ahead (+8 in ARM, +4 in Thumb), the
shifter's carry, interworking. Memory is a set of flat little-endian
regions; an access outside them raises, so a function that touches
hardware says so instead of computing nonsense. SWI, CP15 and undefined
instructions go to handlers that raise unless the caller sets them.

`CPU.call(entry, thumb=False, r0=.., ...)` sets the arguments, points lr at
a return sentinel and runs until the function returns there.

What the architecture leaves to the implementation is done as the DS's two
CPUs do it (and as melonDS does), in the cases a compiler can produce:

* a word load from an unaligned address reads the aligned word rotated
  right by 8 x (address & 3); stores ignore the low bits. A halfword load
  from an odd address reads the aligned halfword on the ARM9 and rotates
  it by 8 on the ARM7, where a signed one reads the signed byte instead;
* str/stm of pc store the instruction's address + 12;
* ldr pc, ldm {..pc} and Thumb pop {pc} interwork on the ARMv5 only;
  mov/add pc never do (the result is aligned for the current state);
* ldm with writeback and the base in the list: on the ARM9 the base is
  written back unless it is the last of several registers loaded; on the
  ARM7 the loaded value stays. stm with writeback stores the old base on
  the ARM9; on the ARM7 the old one only if the base is the lowest
  register;
* the multiplies with S leave C (and V) unchanged; Q exists on the ARMv5.

Cycles, caches, the protection unit and interrupts are not modelled.
"""
import argparse
import struct

from . import arm

RETURN_SENTINEL = 0xFFFFFFF0
M32 = 0xFFFFFFFF

MODES = {0x10: "usr", 0x11: "fiq", 0x12: "irq", 0x13: "svc", 0x17: "abt", 0x1B: "und", 0x1F: "sys"}


def _bank(mode):
    """The register bank a mode uses: usr and sys share one."""
    return "usr" if mode in (0x10, 0x1F) else MODES.get(mode, "usr")


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v


def sat32(x):
    """(saturated value, saturated?) of a signed result."""
    if x > 0x7FFFFFFF:
        return 0x7FFFFFFF, True
    if x < -0x80000000:
        return 0x80000000, True
    return x & M32, False


def shift_c(kind, v, n, c, by_reg):
    """(result, carry out) of the barrel shifter. `n` is the amount as encoded
    (an immediate shift already has lsr/asr #32 as 32 and rrx as kind "rrx"),
    or the bottom byte of Rs when by_reg."""
    if kind is None:
        return v, c
    if kind == "rrx":
        return (c << 31) | (v >> 1), v & 1
    if by_reg and n == 0:
        return v, c
    if kind == "lsl":
        if n < 32:
            return (v << n) & M32, (v >> (32 - n)) & 1
        return 0, (v & 1) if n == 32 else 0
    if kind == "lsr":
        if n < 32:
            return v >> n, (v >> (n - 1)) & 1
        return 0, (v >> 31) if n == 32 else 0
    if kind == "asr":
        if n < 32:
            return (s32(v) >> n) & M32, (v >> (n - 1)) & 1
        return (M32 if v >> 31 else 0), v >> 31
    # ror
    if n & 31 == 0:
        return v, v >> 31
    r = ror(v, n)
    return r, r >> 31


class MemoryFault(Exception):
    pass


class Undefined(Exception):
    pass


class Memory:
    """Flat little-endian regions. `fold(addr)` maps mirrors onto the region
    that holds them (the DS repeats main RAM, the TCMs, WRAM); `io(kind, addr,
    size, value)` answers what no region holds, else the access raises."""

    def __init__(self):
        self.regions = []            # (lo, hi, bytearray)
        self.fold = None
        self.io = None
        self.log = None              # set to a list to record (kind, addr, size, value)

    def add(self, lo, size, data=None):
        buf = bytearray(size)
        if data:
            buf[:len(data)] = data
        self.regions.append((lo, lo + size, buf))
        return buf

    def find(self, addr, size):
        a = addr & M32
        if self.fold:
            a = self.fold(a)
        for lo, hi, buf in self.regions:
            if lo <= a and a + size <= hi:
                return buf, a - lo
        return None, None

    def load(self, addr, data):
        """Copy bytes in, across adjacent regions; raises if any byte has none."""
        done = 0
        while done < len(data):
            buf, o = self.find(addr + done, 1)
            if buf is None:
                raise MemoryFault("loading at %08X: outside memory" % (addr + done))
            n = min(len(data) - done, len(buf) - o)
            buf[o:o + n] = data[done:done + n]
            done += n

    def read(self, addr, size):
        buf, o = self.find(addr, size)
        if buf is None:
            if self.io is None:
                raise MemoryFault("unmapped %d-byte read at %08X" % (size, addr & M32))
            v = self.io("r", addr & M32, size, None)
        else:
            v = int.from_bytes(buf[o:o + size], "little")
        if self.log is not None:
            self.log.append(("r", addr & M32, size, v))
        return v

    def write(self, addr, size, value):
        value &= (1 << (8 * size)) - 1
        buf, o = self.find(addr, size)
        if buf is None:
            if self.io is None:
                raise MemoryFault("unmapped %d-byte write of %X at %08X" % (size, value, addr & M32))
            self.io("w", addr & M32, size, value)
        else:
            buf[o:o + size] = value.to_bytes(size, "little")
        if self.log is not None:
            self.log.append(("w", addr & M32, size, value))


class CPU:
    """arch 5: the ARM9 (ARM946E-S); arch 4: the ARM7 (ARM7TDMI)."""

    def __init__(self, mem, arch=5):
        self.mem, self.arch = mem, arch
        self.r = [0] * 16
        self.pc = 0                  # the instruction being executed
        self.n = self.z = self.c = self.v = self.q = 0
        self.i = self.f = 0
        self.t = 0
        self.mode = 0x1F             # system: the user registers, privileged
        self.banks = {b: [0, 0] for b in ("usr", "fiq", "irq", "svc", "abt", "und")}
        self.fiq_hi = [0] * 5        # r8-r12 of the other side (FIQ's while not in FIQ, and back)
        self.spsr = {b: 0 for b in ("fiq", "irq", "svc", "abt", "und")}
        self.steps = 0
        self.swi = None              # swi(cpu, imm): the BIOS; None raises
        self.cp15_read = None        # cp15_read(cpu, op1, crn, crm, op2) -> value
        self.cp15_write = None       # cp15_write(cpu, op1, crn, crm, op2, value)
        self.cp15 = {}               # what was written, when no handler is set
        self._decoded = {}
        self.npc = 0

    # ---- the status registers -----------------------------------------
    @property
    def cpsr(self):
        return (self.n << 31 | self.z << 30 | self.c << 29 | self.v << 28 | self.q << 27 |
                self.i << 7 | self.f << 6 | self.t << 5 | self.mode)

    def set_cpsr(self, v, mask=0xF, keep_t=True):
        """Write the CPSR's fields (mask: c=1, x=2, s=4, f=8); a new mode
        switches the banked registers. In user mode only the flags change."""
        if mask & 8:
            self.n, self.z, self.c, self.v = v >> 31 & 1, v >> 30 & 1, v >> 29 & 1, v >> 28 & 1
            if self.arch >= 5:
                self.q = v >> 27 & 1
        if mask & 1 and self.mode != 0x10:
            self.i, self.f = v >> 7 & 1, v >> 6 & 1
            if not keep_t:
                self.t = v >> 5 & 1
            self.switch_mode((v & 0x1F) | 0x10)

    def switch_mode(self, new):
        old = self.mode
        if new == old:
            return
        ob, nb = _bank(old), _bank(new)
        if ob != nb:
            self.banks[ob] = [self.r[13], self.r[14]]
            if (ob == "fiq") != (nb == "fiq"):
                self.r[8:13], self.fiq_hi = self.fiq_hi, self.r[8:13]
            self.r[13], self.r[14] = self.banks[nb]
        self.mode = new

    def get_spsr(self):
        b = _bank(self.mode)
        return self.spsr[b] if b != "usr" else self.cpsr

    def set_spsr(self, v, mask=0xF):
        b = _bank(self.mode)
        if b == "usr":
            return
        m = (0xFF if mask & 1 else 0) | (0xFF00 if mask & 2 else 0) | \
            (0xFF0000 if mask & 4 else 0) | (0xFF000000 if mask & 8 else 0)
        self.spsr[b] = (self.spsr[b] & ~m | v & m) & M32

    def restore_cpsr(self):
        """CPSR = SPSR (an exception return: subs pc, lr / ldm ..^ with pc)."""
        if _bank(self.mode) != "usr":
            self.set_cpsr(self.get_spsr(), 0xF, keep_t=False)

    def user_reg(self, n):
        """Register n as user mode sees it (ldm/stm with ^)."""
        b = _bank(self.mode)
        if n >= 13 and b != "usr":
            return self.banks["usr"][n - 13]
        if 8 <= n <= 12 and b == "fiq":
            return self.fiq_hi[n - 8]
        return self.r[n]

    def set_user_reg(self, n, v):
        b = _bank(self.mode)
        if n >= 13 and b != "usr":
            self.banks["usr"][n - 13] = v
        elif 8 <= n <= 12 and b == "fiq":
            self.fiq_hi[n - 8] = v
        else:
            self.r[n] = v

    # ---- memory ---------------------------------------------------------
    def ld32(self, a):
        """A word load: unaligned addresses read the aligned word rotated."""
        v = self.mem.read(a & ~3, 4)
        return ror(v, (a & 3) * 8)

    def ld16(self, a, signed=False):
        if self.arch >= 5 or not a & 1:
            v = self.mem.read(a & ~1, 2)
            return (s16(v) & M32) if signed else v
        if signed:                   # ARM7, odd: the signed byte
            b = self.mem.read(a, 1)
            return (b - 0x100 if b & 0x80 else b) & M32
        return ror(self.mem.read(a & ~1, 2), 8)

    # ---- running --------------------------------------------------------
    def fetch(self, pc, thumb):
        if thumb:
            raw = self.mem.read(pc, 2)
            try:
                nxt = self.mem.read(pc + 2, 2) if raw >> 11 == 30 else None
            except MemoryFault:
                nxt = None
            key = (raw, nxt)
        else:
            key = self.mem.read(pc, 4)
        hit = self._decoded.get((pc, thumb))
        if hit is not None and hit[0] == key:
            return hit[1]
        if thumb:
            ins = arm.decode_thumb(raw, pc, nxt, self.arch)
        else:
            ins = arm.decode_arm(key, pc, self.arch)
        self._decoded[(pc, thumb)] = (key, ins)
        return ins

    def call(self, entry, thumb=None, max_steps=10_000_000, **regs):
        """Run the function at `entry` (bit 0 or `thumb` for Thumb) until it
        returns to the sentinel put in lr; returns r0."""
        for k, v in regs.items():
            self.r[int(k[1:])] = v & M32
        if thumb is None:
            thumb = bool(entry & 1)
        self.r[14] = RETURN_SENTINEL
        self.pc, self.t = entry & ~1, int(thumb)
        limit = self.steps + max_steps
        while self.pc != RETURN_SENTINEL:
            self.step()
            if self.steps > limit:
                raise RuntimeError("no return after %d steps (pc %08X)" % (max_steps, self.pc))
        return self.r[0]

    def step(self):
        pc, thumb = self.pc, self.t
        ins = self.fetch(pc, thumb)
        self.steps += 1
        self.npc = pc + ins.size
        if ins.cond == 14 or self.cond(ins.cond):
            fn = _EXEC.get(ins.op)
            if fn is None:
                raise Undefined("%08X: %s" % (pc, ins.text))
            fn(self, ins)
        self.pc = self.npc & M32

    def cond(self, cc):
        n, z, c, v = self.n, self.z, self.c, self.v
        if cc == 0: return z
        if cc == 1: return not z
        if cc == 2: return c
        if cc == 3: return not c
        if cc == 4: return n
        if cc == 5: return not n
        if cc == 6: return v
        if cc == 7: return not v
        if cc == 8: return c and not z
        if cc == 9: return not c or z
        if cc == 10: return n == v
        if cc == 11: return n != v
        if cc == 12: return not z and n == v
        if cc == 13: return z or n != v
        return cc == 14

    def rv(self, n, ins, extra=0):
        """Register n as an operand: pc reads the instruction's address + 8 (ARM)
        or + 4 (Thumb); `extra` 4 for an ARM register-specified shift."""
        if n == 15:
            return (ins.pc + (4 if ins.thumb else 8 + extra)) & M32
        return self.r[n]

    def branch(self, target, interwork):
        """pc = target; with interwork bit 0 picks the state."""
        target &= M32
        if interwork:
            self.t = target & 1
        self.npc = target & (~1 if self.t else ~3)

    def write_pc(self, value, ins):
        """A load into pc: interworks on the ARMv5."""
        self.branch(value, self.arch >= 5)


# ---------------------------------------------------------------- execution


def _op2(cpu, ins):
    """(value, carry out) of a data-processing instruction's second operand."""
    if ins.imm is not None:
        v = ins.imm & M32
        return v, (v >> 31 if ins.rot else cpu.c)
    if ins.rs is not None:
        extra = 0 if ins.thumb else 4
        v = cpu.rv(ins.rm, ins, extra)
        return shift_c(ins.shift, v, cpu.r[ins.rs] & 0xFF, cpu.c, True)
    return shift_c(ins.shift, cpu.rv(ins.rm, ins), ins.shift_imm, cpu.c, False)


def _rn(cpu, ins):
    if ins.rn == 15 and ins.thumb and ins.imm is not None:
        return (ins.pc + 4) & ~3              # adr / ldr literal: the word-aligned pc
    return cpu.rv(ins.rn, ins, 4 if ins.rs is not None and not ins.thumb else 0)


def _nz(cpu, r):
    cpu.n, cpu.z = r >> 31, int(r == 0)


def _dp_result(cpu, ins, r):
    if ins.rd == 15:
        if ins.s:                             # subs pc, lr, #4...: an exception return
            cpu.restore_cpsr()
        cpu.branch(r, False)
    else:
        cpu.r[ins.rd] = r


def _logic(fn):
    def ex(cpu, ins):
        b, carry = _op2(cpu, ins)
        r = fn(cpu, ins, b) & M32
        if ins.s and ins.rd != 15:
            _nz(cpu, r)
            cpu.c = carry
        _dp_result(cpu, ins, r)
    return ex


def _test(fn):
    def ex(cpu, ins):
        b, carry = _op2(cpu, ins)
        r = fn(cpu, ins, b) & M32
        _nz(cpu, r)
        cpu.c = carry
    return ex


def _arith(kind, write=True):
    """add/adc/sub/sbc/rsb/rsc and cmp/cmn: a + b + carry_in with the flags."""
    def ex(cpu, ins):
        b, _ = _op2(cpu, ins)
        a = _rn(cpu, ins)
        if kind in ("rsb", "rsc"):
            a, b = b, a
        if kind in ("add", "cmn"):
            x, y, ci = a, b, 0
        elif kind == "adc":
            x, y, ci = a, b, cpu.c
        elif kind in ("sub", "rsb", "cmp"):
            x, y, ci = a, ~b & M32, 1
        else:                                 # sbc, rsc
            x, y, ci = a, ~b & M32, cpu.c
        full = x + y + ci
        r = full & M32
        if not write or (ins.s and ins.rd != 15):
            _nz(cpu, r)
            cpu.c = full >> 32
            cpu.v = ((x ^ r) & (y ^ r)) >> 31
        if write:
            _dp_result(cpu, ins, r)
    return ex


def _mul(cpu, ins):
    r = (cpu.r[ins.rm] * cpu.r[ins.rs]) & M32
    if ins.op == "mla":
        r = (r + cpu.r[ins.rn]) & M32
    cpu.r[ins.rd] = r
    if ins.s:
        _nz(cpu, r)


def _mull(cpu, ins):
    a, b = cpu.r[ins.rm], cpu.r[ins.rs]
    if ins.op[0] == "s":
        a, b = s32(a), s32(b)
    p = a * b
    if ins.op.endswith("lal"):
        p += cpu.r[ins.rd2] << 32 | cpu.r[ins.rd]
    p &= (1 << 64) - 1
    cpu.r[ins.rd], cpu.r[ins.rd2] = p & M32, p >> 32
    if ins.s:
        cpu.n, cpu.z = p >> 63, int(p == 0)


def _half(v, top):
    return s16(v >> 16) if top else s16(v)


def _dsp_mul(cpu, ins):
    op = ins.op
    if op.startswith("smlal"):
        p = _half(cpu.r[ins.rm], op[5] == "t") * _half(cpu.r[ins.rs], op[6] == "t")
        acc = (cpu.r[ins.rd2] << 32 | cpu.r[ins.rd]) + p
        acc &= (1 << 64) - 1
        cpu.r[ins.rd], cpu.r[ins.rd2] = acc & M32, acc >> 32
        return
    if op[4] == "w":                          # smlaw<y>, smulw<y>: 32 x 16, the top 32 of 48 bits
        p = (s32(cpu.r[ins.rm]) * _half(cpu.r[ins.rs], op[5] == "t")) >> 16
    else:                                     # smla<x><y>, smul<x><y>: 16 x 16
        p = _half(cpu.r[ins.rm], op[4] == "t") * _half(cpu.r[ins.rs], op[5] == "t")
    if op.startswith("smla"):
        s = s32(p) + s32(cpu.r[ins.rn])
        if not -0x80000000 <= s <= 0x7FFFFFFF:
            cpu.q = 1
        cpu.r[ins.rd] = s & M32
    else:
        cpu.r[ins.rd] = p & M32


def _qarith(cpu, ins):
    a, b = s32(cpu.r[ins.rm]), s32(cpu.r[ins.rn])
    if ins.op in ("qdadd", "qdsub"):
        b2, sat = sat32(2 * b)
        if sat:
            cpu.q = 1
        b = s32(b2)
    r, sat = sat32(a + b if ins.op in ("qadd", "qdadd") else a - b)
    if sat:
        cpu.q = 1
    cpu.r[ins.rd] = r


def _clz(cpu, ins):
    v = cpu.r[ins.rm]
    cpu.r[ins.rd] = 32 - v.bit_length()


def _addr(cpu, ins):
    """(address accessed, base after writeback) of a single load/store."""
    base = _rn(cpu, ins)
    if ins.rm is not None:
        off, _ = shift_c(ins.shift, cpu.rv(ins.rm, ins), ins.shift_imm, cpu.c, False)
    else:
        off = ins.imm or 0
    moved = (base + off if ins.add else base - off) & M32
    return (moved if ins.pre else base), moved


def _wb(cpu, ins, moved):
    if ins.writeback and ins.rn != 15:
        cpu.r[ins.rn] = moved


def _load(cpu, ins):
    a, moved = _addr(cpu, ins)
    op = ins.op
    if op in ("ldr", "ldrt"):
        v = cpu.ld32(a)
    elif op in ("ldrb", "ldrbt"):
        v = cpu.mem.read(a, 1)
    elif op == "ldrh":
        v = cpu.ld16(a)
    elif op == "ldrsh":
        v = cpu.ld16(a, signed=True)
    elif op == "ldrsb":
        v = cpu.mem.read(a, 1)
        v = (v - 0x100 if v & 0x80 else v) & M32
    else:                                     # ldrd
        lo, hi = cpu.mem.read(a & ~3, 4), cpu.mem.read((a & ~3) + 4, 4)
        _wb(cpu, ins, moved)
        cpu.r[ins.rd], cpu.r[ins.rd + 1] = lo, hi
        return
    _wb(cpu, ins, moved)                      # the loaded value wins over the base
    if ins.rd == 15:
        cpu.write_pc(v, ins)
    else:
        cpu.r[ins.rd] = v


def _store(cpu, ins):
    a, moved = _addr(cpu, ins)
    op = ins.op
    v = cpu.rv(ins.rd, ins, 4)                # str pc: address + 12
    if op in ("str", "strt"):
        cpu.mem.write(a & ~3, 4, v)
    elif op in ("strb", "strbt"):
        cpu.mem.write(a, 1, v)
    elif op == "strh":
        cpu.mem.write(a & ~1, 2, v)
    else:                                     # strd
        cpu.mem.write(a & ~3, 4, cpu.r[ins.rd])
        cpu.mem.write((a & ~3) + 4, 4, cpu.rv(ins.rd + 1, ins, 4))
    _wb(cpu, ins, moved)


def _block(ins, base):
    """(first address, base after writeback) of ldm/stm."""
    n = bin(ins.reglist).count("1")
    mode = ins.mode
    if mode == "ia":
        return base, base + 4 * n
    if mode == "ib":
        return base + 4, base + 4 * n
    if mode == "da":
        return base - 4 * n + 4, base - 4 * n
    return base - 4 * n, base - 4 * n         # db


def _ldm(cpu, ins):
    rl, rn = ins.reglist, ins.rn
    base = cpu.r[rn]
    a, new = _block(ins, base)
    a &= ~3
    user = ins.user and not rl & 0x8000       # ^ without pc: the user registers
    vals = []
    for i in range(16):
        if rl >> i & 1:
            vals.append((i, cpu.mem.read(a, 4)))
            a += 4
    for i, v in vals:
        if i == 15:
            continue
        if user:
            cpu.set_user_reg(i, v)
        else:
            cpu.r[i] = v
    if ins.writeback:
        if not rl >> rn & 1:
            cpu.r[rn] = new & M32
        elif cpu.arch >= 5 and not ins.thumb and (rl == 1 << rn or rl >> (rn + 1)):
            cpu.r[rn] = new & M32             # ARM9: written back unless it is the last loaded
    if rl & 0x8000:
        v = vals[-1][1]
        if ins.user:                          # ldm ..., {..., pc}^: an exception return
            cpu.restore_cpsr()
            cpu.branch(v, False)
        else:
            cpu.write_pc(v, ins)


def _stm(cpu, ins):
    rl, rn = ins.reglist, ins.rn
    base = cpu.r[rn]
    a, new = _block(ins, base)
    a &= ~3
    lowest = (rl & -rl).bit_length() - 1
    for i in range(16):
        if rl >> i & 1:
            if ins.user:
                v = cpu.user_reg(i)
            elif i == 15:
                v = cpu.rv(15, ins, 4)
            elif i == rn and ins.writeback and cpu.arch < 5 and i != lowest:
                v = new & M32                 # ARM7: the new base unless it comes first
            else:
                v = cpu.r[i]
            cpu.mem.write(a, 4, v)
            a += 4
    if ins.writeback:
        cpu.r[rn] = new & M32


def _swp(cpu, ins):
    a = cpu.r[ins.rn]
    if ins.op == "swpb":
        v = cpu.mem.read(a, 1)
        cpu.mem.write(a, 1, cpu.r[ins.rm])
    else:
        v = cpu.ld32(a)
        cpu.mem.write(a & ~3, 4, cpu.r[ins.rm])
    cpu.r[ins.rd] = v


def _b(cpu, ins):
    cpu.npc = ins.target


def _bl(cpu, ins):
    cpu.r[14] = (ins.pc + ins.size) | (1 if ins.thumb else 0)
    if ins.op == "blx":                       # immediate: the other state
        cpu.t = 0 if ins.thumb else 1
    cpu.npc = ins.target


def _bx(cpu, ins):
    x = cpu.rv(ins.rm, ins)
    if ins.op == "blx":
        cpu.r[14] = (ins.pc + ins.size) | (1 if ins.thumb else 0)
    cpu.branch(x, True)


def _bl_hi(cpu, ins):
    cpu.r[14] = (ins.pc + 4 + ins.imm) & M32


def _bl_lo(cpu, ins):
    t = (cpu.r[14] + ins.imm) & M32
    cpu.r[14] = (ins.pc + 2) | 1
    if ins.op == "blx_lo":
        cpu.t = 0
        cpu.npc = t & ~3
    else:
        cpu.npc = t & ~1


def _mrs(cpu, ins):
    cpu.r[ins.rd] = cpu.get_spsr() if ins.spsr else cpu.cpsr


def _msr(cpu, ins):
    v = ins.imm if ins.imm is not None else cpu.r[ins.rm]
    if ins.spsr:
        cpu.set_spsr(v, ins.mask)
    else:
        cpu.set_cpsr(v, ins.mask)


def _swi(cpu, ins):
    if cpu.swi is None:
        raise Undefined("%08X: swi %X (no BIOS)" % (ins.pc, ins.imm))
    cpu.swi(cpu, ins.imm)


def _mcr(cpu, ins):
    if ins.cp != 15 or ins.op.endswith("2"):
        raise Undefined("%08X: %s" % (ins.pc, ins.text))
    key = (ins.op1, ins.crn, ins.crm, ins.op2)
    if ins.op == "mcr":
        v = cpu.rv(ins.rd, ins, 4)
        if cpu.cp15_write:
            cpu.cp15_write(cpu, *key, v)
        else:
            cpu.cp15[key] = v
        return
    v = cpu.cp15_read(cpu, *key) if cpu.cp15_read else cpu.cp15.get(key, 0)
    if ins.rd == 15:
        cpu.n, cpu.z, cpu.c, cpu.v = v >> 31 & 1, v >> 30 & 1, v >> 29 & 1, v >> 28 & 1
    else:
        cpu.r[ins.rd] = v & M32


def _nop(cpu, ins):
    pass


def _undef(cpu, ins):
    raise Undefined("%08X: %s" % (ins.pc, ins.text))


_EXEC = {
    "and": _logic(lambda c, i, b: _rn(c, i) & b),
    "eor": _logic(lambda c, i, b: _rn(c, i) ^ b),
    "orr": _logic(lambda c, i, b: _rn(c, i) | b),
    "bic": _logic(lambda c, i, b: _rn(c, i) & ~b),
    "mov": _logic(lambda c, i, b: b),
    "mvn": _logic(lambda c, i, b: ~b),
    "tst": _test(lambda c, i, b: _rn(c, i) & b),
    "teq": _test(lambda c, i, b: _rn(c, i) ^ b),
    "add": _arith("add"), "adc": _arith("adc"), "sub": _arith("sub"), "sbc": _arith("sbc"),
    "rsb": _arith("rsb"), "rsc": _arith("rsc"),
    "cmp": _arith("cmp", False), "cmn": _arith("cmn", False),
    "mul": _mul, "mla": _mul,
    "umull": _mull, "umlal": _mull, "smull": _mull, "smlal": _mull,
    "qadd": _qarith, "qsub": _qarith, "qdadd": _qarith, "qdsub": _qarith,
    "clz": _clz,
    "ldr": _load, "ldrb": _load, "ldrh": _load, "ldrsb": _load, "ldrsh": _load,
    "ldrt": _load, "ldrbt": _load, "ldrd": _load,
    "str": _store, "strb": _store, "strh": _store, "strt": _store, "strbt": _store, "strd": _store,
    "ldm": _ldm, "stm": _stm, "swp": _swp, "swpb": _swp,
    "b": _b, "bl": _bl, "bx": _bx,
    "bl_hi": _bl_hi, "bl_lo": _bl_lo, "blx_lo": _bl_lo,
    "mrs": _mrs, "msr": _msr, "swi": _swi,
    "mcr": _mcr, "mrc": _mcr, "mcr2": _mcr, "mrc2": _mcr,
    "pld": _nop, "nop": _nop,
    "bkpt": _undef, "undefined": _undef,
}
for _x in "bt":
    for _y in "bt":
        _EXEC["smul" + _x + _y] = _EXEC["smla" + _x + _y] = _EXEC["smlal" + _x + _y] = _dsp_mul
    _EXEC["smulw" + _x] = _EXEC["smlaw" + _x] = _dsp_mul


def _blx(cpu, ins):
    if ins.target is not None:
        _bl(cpu, ins)
    else:
        _bx(cpu, ins)


_EXEC["blx"] = _blx


# ---------------------------------------------------------------- the DS's memory


def ds_memory(cpu=9):
    """Empty DS memory as the recompiler's runtime lays it out for the
    self-test: main RAM (4 MiB, repeated up to 0x02FFFFFF); on the ARM9 the
    ITCM (32 KiB at 0x01FF8000, repeated from 0) and the DTCM (16 KiB at
    0x027E0000, over main RAM); on the ARM7 its WRAM (64 KiB at 0x03800000,
    repeated to 0x03FFFFFF) and the shared WRAM (32 KiB, repeated over
    0x03000000-0x037FFFFF), all of it the ARM7's as the SDK's boot leaves it
    (WRAMCNT = 3)."""
    mem = Memory()
    mem.add(0x02000000, 0x400000)
    if cpu == 9:
        mem.add(0x01FF8000, 0x8000)
        mem.add(0x027E0000, 0x4000)

        def fold(a):
            if 0x027E0000 <= a < 0x027E4000:
                return a
            if a < 0x02000000:
                return 0x01FF8000 | (a & 0x7FFF)
            if a >> 24 == 2:
                return 0x02000000 | (a & 0x3FFFFF)
            return a
    else:
        mem.add(0x03800000, 0x10000)
        mem.add(0x03000000, 0x8000)

        def fold(a):
            if a >> 24 == 2:
                return 0x02000000 | (a & 0x3FFFFF)
            if a >> 23 == 7:                  # 0x03800000-0x03FFFFFF
                return 0x03800000 | (a & 0xFFFF)
            if a >> 23 == 6:                  # 0x03000000-0x037FFFFF
                return 0x03000000 | (a & 0x7FFF)
            return a
    mem.fold = fold
    return mem


def load_program(mem, segments):
    """Copy a program's segments (recomp.discover.Segment) to where they run."""
    for s in segments:
        mem.load(s.base, s.data)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help=".nds (with --cpu) or a flat binary (with --base)")
    ap.add_argument("--cpu", type=int, choices=(7, 9), default=9)
    ap.add_argument("--base", help="flat binary: load address (hex)")
    ap.add_argument("--call", required=True, help="the function (hex; odd = Thumb)")
    ap.add_argument("--thumb", action="store_true")
    ap.add_argument("--regs", default="", help="r0=HEX,r1=HEX,... (sp defaults to the top of DTCM/WRAM)")
    ap.add_argument("--trace", action="store_true", help="print every instruction")
    a = ap.parse_args(argv)
    from .recomp import discover
    with open(a.file, "rb") as fh:
        raw = fh.read()
    if a.base is None:
        from .rom import Rom
        segs, _ = discover.segments_from_rom(Rom(raw), a.cpu)
    else:
        segs = [discover.Segment(int(a.base, 16), raw, "main")]
    mem = ds_memory(a.cpu)
    load_program(mem, segs)
    cpu = CPU(mem, arch=5 if a.cpu == 9 else 4)
    cpu.r[13] = 0x027E3F00 if a.cpu == 9 else 0x0380FF00
    regs = {}
    for kv in a.regs.split(","):
        if kv:
            k, v = kv.split("=")
            regs["r13" if k == "sp" else "r14" if k == "lr" else k] = int(v, 16)
    entry = int(a.call, 16)
    if a.trace:
        step = cpu.step

        def traced():
            print(repr(cpu.fetch(cpu.pc, cpu.t)))
            step()
        cpu.step = traced
    cpu.call(entry, True if a.thumb else None, **regs)
    print("returned after %d instructions" % cpu.steps)
    print("  ".join("%s=%08X" % (arm.REGS[i], cpu.r[i]) for i in range(8)))
    print("  ".join("%s=%08X" % (arm.REGS[i], cpu.r[i]) for i in range(8, 16)))
    print("cpsr=%08X (N%d Z%d C%d V%d Q%d)" % (cpu.cpsr, cpu.n, cpu.z, cpu.c, cpu.v, cpu.q))


if __name__ == "__main__":
    main()
