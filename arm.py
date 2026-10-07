"""ARM9 / ARM7 (ARMv5TE / ARMv4T) decoding, disassembly and cross-references.

    python -m ndskit.arm FILE --base 02000000 --at 02000800 [--count 64] [--thumb]
    python -m ndskit.arm FILE --base 02000000 --census [--thumb]
    python -m ndskit.arm FILE --base 02000000 --refs 04000000:04800000
    python -m ndskit.arm GAME.nds --offset 4000 --size SIZE --base 02000000 --at 02000800
    (--arm7 decodes for the ARM7TDMI: the ARMv5 additions become "undefined")

The DS has two CPUs. The ARM9 is an ARM946E-S (ARMv5TE: adds CLZ, BLX,
BKPT, the DSP multiplies SMLAxy/SMULxy/SMLAWy/SMULWy/SMLALxy, QADD/QSUB/
QDADD/QDSUB, LDRD/STRD, PLD, MCRR/MRRC and the CDP2/LDC2/MCR2... forms);
the ARM7 is an ARM7TDMI (ARMv4T). Both are little-endian and run ARM code
(32-bit, word-aligned) and Thumb code (16-bit, halfword-aligned); bit 0 of a
BX/BLX target picks the mode. On ARMv5 a load into pc (ldr pc, ldm {..pc},
Thumb pop {pc}) also interworks; on the ARM7 it does not, and mov/add pc
never does. The ARM9 sets up its caches, TCMs and protection unit with
mcr p15.

PC reads as the instruction address + 8 in ARM, + 4 in Thumb. ARM literal
loads are ldr rX,[pc,#+-imm12] (literal at pc+8+-imm); Thumb ones are
ldr rX,[pc,#imm8*4] (literal at ((pc+4) & ~3) + imm*4). They are how code
builds 32-bit constants, so they are where addresses (I/O registers, data,
function pointers) are found: `refs` lists them. Thumb BL/BLX is a pair of
halfwords (0xF000 prefix holding the high offset, then a 0xF800 BL or 0xE800
BLX suffix) and decodes as one 4-byte instruction when both are given; alone
the halves decode as "bl_hi" / "bl_lo" / "blx_lo".

decode_arm(word, pc) / decode_thumb(hw, pc, next_hw) return an Insn with
.op (canonical mnemonic without condition or S suffix: "mov", "ldr", "ldm",
"bl"; Thumb instructions get the ARM op they behave as, e.g. lsls r0,r1,#2
is op "mov" with .shift "lsl"), .cond (0-14, 14 = always), .s (sets flags),
registers .rd .rn .rm .rs (.rd2: RdHi of long multiplies, second register
of ldrd/strd), .imm, .rot, .shift/.shift_imm (or .rs for a register shift),
memory .pre .add .writeback .user, .reglist (bitmask) and .mode (ia/ib/da/db)
for ldm/stm, PSR .spsr .mask, coprocessor .cp .op1 .op2 .crd .crn .crm .long,
.target (branch target, or the address a pc-relative load reads / an
add rX,pc computes), .exchange (None, "arm"/"thumb" when the new mode is
static, "dynamic" when bit 0 of the loaded value decides), and the
properties .is_branch, .is_call, .writes_pc, .literal_size and .text
(capstone-like syntax).

Should-be-zero/one fields: checked for MRS, MSR, BX, BLX, CLZ and PLD
(later architectures reuse that space), ignored elsewhere (Rn of mov/mvn,
Rd of compares, Thumb bx bits 2-0), as the hardware does.

Checked against capstone 5.0.7 (scratch script, not shipped; text compared
after normalising aliases such as push/stmdb sp!, svc/swi, sb/r9, lsl/mov,
hex/decimal). Thumb, all 65 536 halfwords (BL prefixes tried with both a BL
and a BLX suffix: 67 584 cases, at a word- and a halfword-aligned pc):
61 838 agree (60 478 same text, 1 360 invalid for both); 3 072 are lone
BL/BLX suffixes, which capstone reads as Thumb-2 32-bit prefixes; 2 498 are
v6/v7 instructions (cbz, it, sxth, rev, cps, setend, hints, Thumb-2) that
are undefined here; 176 have the SBZ bits of bx/blx set (capstone: v8-M
bxns/blxns, or invalid). The 969 BL/BLX pairs in the Moon binaries all
agree. ARM, the 386 110 words of the Moon ARM9 and ARM7 binaries / 1 000 000
random words: 385 054 / 919 058 agree (same text or invalid for both); the
rest are ARMv6/v7 encodings (ldrex, movw, sxtb, media, rfe, pli, VFP/NEON:
824 / 52 349, undefined or a coprocessor instruction here), cp10/cp11
coprocessor instructions capstone rejects as invalid VFP (35 / 9 634),
mov/mvn with the SBZ Rn field set, which capstone rejects except for the
plain register form (57 / 18 795), mrs with wrong SBO/SBZ bits, which
capstone accepts (140 / 148), and msr-with-no-field plus a rotate (0 / 16,
v6K hint space). All 206 600 branch targets and literal addresses compared
agree numerically.
"""
import argparse
import collections
import struct

from . import hw

REGS = ("r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc")
CONDS = ("eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
         "hi", "ls", "ge", "lt", "gt", "le", "", "")
DP_OPS = ("and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
          "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn")
SHIFTS = ("lsl", "lsr", "asr", "ror")
COMPARES = frozenset(("tst", "teq", "cmp", "cmn"))
DATA_OPS = frozenset(DP_OPS)
LOADS = frozenset(("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrt", "ldrbt", "ldrd"))
STORES = frozenset(("str", "strb", "strh", "strt", "strbt", "strd"))
_BRANCHES = frozenset(("b", "bl", "blx", "bx", "bl_lo", "blx_lo"))
_CALLS = frozenset(("bl", "blx", "bl_lo", "blx_lo"))
_WRITES_RD = (DATA_OPS - COMPARES) | (LOADS - {"ldrd"})
_LIT_SIZE = {"ldr": 4, "ldrb": 1, "ldrsb": 1, "ldrh": 2, "ldrsh": 2, "ldrd": 8}
_MULS = frozenset(("mul", "mla"))
_LONG_MULS = frozenset(("umull", "umlal", "smull", "smlal"))


class Insn:
    __slots__ = ("pc", "raw", "size", "thumb", "op", "cond", "s",
                 "rd", "rd2", "rn", "rm", "rs", "imm", "rot", "shift", "shift_imm",
                 "pre", "add", "writeback", "user", "reglist", "mode",
                 "spsr", "mask", "cp", "op1", "op2", "crd", "crn", "crm", "long",
                 "target", "exchange")

    def __init__(self, pc, raw, size, thumb, op, cond=14, **kw):
        self.pc, self.raw, self.size, self.thumb, self.op, self.cond = pc, raw, size, thumb, op, cond
        self.s = False
        self.rd = self.rd2 = self.rn = self.rm = self.rs = None
        self.imm = self.shift = None
        self.rot = self.shift_imm = 0
        self.pre = self.add = self.writeback = self.user = None
        self.reglist = self.mode = self.spsr = self.mask = None
        self.cp = self.op1 = self.op2 = self.crd = self.crn = self.crm = self.long = None
        self.target = self.exchange = None
        for k, v in kw.items():
            setattr(self, k, v)

    @property
    def is_branch(self):
        """b/bl/blx/bx (and the lone second half of a Thumb BL/BLX)."""
        return self.op in _BRANCHES

    @property
    def is_call(self):
        """bl/blx: writes lr with the return address."""
        return self.op in _CALLS

    @property
    def writes_pc(self):
        """Any instruction that writes r15: branches, mov/add/... pc,
        ldr pc, ldm/pop with pc in the list."""
        if self.op in _BRANCHES:
            return True
        if self.op == "ldm":
            return bool(self.reglist & 0x8000)
        return self.rd == 15 and self.op in _WRITES_RD

    @property
    def literal_size(self):
        """Bytes a pc-relative load reads at .target, else None."""
        if self.rn == 15 and self.target is not None and self.op in _LIT_SIZE:
            return _LIT_SIZE[self.op]
        return None

    @property
    def text(self):
        return _text(self)

    def __repr__(self):
        if not self.thumb:
            raw = "%08X " % self.raw
        elif self.size == 4:
            raw = "%04X %04X" % (self.raw >> 16, self.raw & 0xFFFF)
        else:
            raw = "%04X     " % self.raw
        return "%08X  %s  %s" % (self.pc, raw, self.text)


def _ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _undef(w, pc, cond, thumb=False, size=4):
    return Insn(pc, w, size, thumb, "undefined", cond)


# ---------------------------------------------------------------- ARM


def _imm_shift(w):
    """(shift, amount) of an immediate-shifted register operand (bits 11-5)."""
    amount, t = (w >> 7) & 31, (w >> 5) & 3
    if t == 0:
        return ("lsl", amount) if amount else (None, 0)
    if t == 3 and amount == 0:
        return "rrx", 0
    return SHIFTS[t], amount or 32           # lsr/asr #0 encode #32


def _arm_dp(w, pc, cond, imm_form):
    opc, rn, rd = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15
    op = DP_OPS[opc]
    ins = Insn(pc, w, 4, False, op, cond, s=bool(w & 0x00100000))
    if imm_form:
        ins.rot = ((w >> 8) & 15) * 2
        ins.imm = _ror(w & 0xFF, ins.rot)
        if rn == 15 and op in ("add", "sub") and not ins.s:
            ins.target = (pc + 8 + (ins.imm if op == "add" else -ins.imm)) & 0xFFFFFFFF
    else:
        ins.rm = w & 15
        if w & 0x10:                          # register-specified shift
            ins.shift, ins.rs = SHIFTS[(w >> 5) & 3], (w >> 8) & 15
        else:
            ins.shift, ins.shift_imm = _imm_shift(w)
    if op not in COMPARES:                    # Rd field SBZ for compares
        ins.rd = rd
    if op not in ("mov", "mvn"):              # Rn field SBZ for moves
        ins.rn = rn
    return ins


def _arm_misc(w, pc, cond, arch):
    """Opcode 10xx with S=0 in the data-processing space: PSR transfers,
    BX/BLX, CLZ, saturating adds, BKPT and the DSP multiplies.

    The should-be-one/zero fields of MRS, MSR, BX, BLX and CLZ are checked
    (later architectures reuse that space: banked MRS/MSR, BXJ...); a
    violation decodes as "undefined", as capstone does."""
    op, b74 = (w >> 21) & 3, (w >> 4) & 15
    rd, rn, rs, rm = (w >> 12) & 15, (w >> 16) & 15, (w >> 8) & 15, w & 15
    if b74 == 0:
        if not op & 1:                        # (bit 9 set: banked MRS, v7VE)
            if w & 0x000F0FFF != 0x000F0000:
                return _undef(w, pc, cond)
            return Insn(pc, w, 4, False, "mrs", cond, rd=rd, spsr=bool(op & 2))
        if w & 0xFFF0 != 0xF000:
            return _undef(w, pc, cond)
        return Insn(pc, w, 4, False, "msr", cond, rm=rm, spsr=bool(op & 2), mask=rn)
    sbo = w & 0x000FFF00 == 0x000FFF00        # bx/blx: bits 19-8 should be one
    if b74 == 1 and op == 1 and sbo:
        return Insn(pc, w, 4, False, "bx", cond, rm=rm, exchange="dynamic")
    if arch < 5:
        return _undef(w, pc, cond)
    if b74 == 1 and op == 3 and w & 0x000F0F00 == 0x000F0F00:
        return Insn(pc, w, 4, False, "clz", cond, rd=rd, rm=rm)
    if b74 == 3 and op == 1 and sbo:
        return Insn(pc, w, 4, False, "blx", cond, rm=rm, exchange="dynamic")
    if b74 == 5:
        return Insn(pc, w, 4, False, ("qadd", "qsub", "qdadd", "qdsub")[op], cond,
                    rd=rd, rn=rn, rm=rm)
    if b74 == 7 and op == 1 and cond == 14:
        return Insn(pc, w, 4, False, "bkpt", cond, imm=((w >> 4) & 0xFFF0) | (w & 15))
    if b74 & 9 == 8:                          # 1yx0: signed 16-bit multiplies
        x, y = "bt"[(w >> 5) & 1], "bt"[(w >> 6) & 1]
        # here bits 19-16 are Rd (RdHi) and bits 15-12 Rn (RdLo)
        if op == 0:
            return Insn(pc, w, 4, False, "smla" + x + y, cond, rd=rn, rn=rd, rs=rs, rm=rm)
        if op == 1:
            if x == "b":
                return Insn(pc, w, 4, False, "smlaw" + y, cond, rd=rn, rn=rd, rs=rs, rm=rm)
            return Insn(pc, w, 4, False, "smulw" + y, cond, rd=rn, rs=rs, rm=rm)
        if op == 2:
            return Insn(pc, w, 4, False, "smlal" + x + y, cond, rd=rd, rd2=rn, rs=rs, rm=rm)
        return Insn(pc, w, 4, False, "smul" + x + y, cond, rd=rn, rs=rs, rm=rm)
    return _undef(w, pc, cond)


def _arm_mul_extra(w, pc, cond, arch):
    """Bits 7 and 4 set: multiplies, SWP, and the halfword/signed/double
    loads and stores."""
    sh = (w >> 5) & 3
    if sh == 0:
        rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        if not w & 0x0F000000:
            op, s = (w >> 21) & 7, bool(w & 0x00100000)
            if op == 0:
                return Insn(pc, w, 4, False, "mul", cond, s=s, rd=rd, rm=rm, rs=rs)
            if op == 1:
                return Insn(pc, w, 4, False, "mla", cond, s=s, rd=rd, rn=rn, rm=rm, rs=rs)
            if op >= 4:                       # RdLo in bits 15-12, RdHi in 19-16
                return Insn(pc, w, 4, False, ("umull", "umlal", "smull", "smlal")[op - 4],
                            cond, s=s, rd=rn, rd2=rd, rm=rm, rs=rs)
            return _undef(w, pc, cond)        # umaal, mls (v6)
        if w & 0x0FB00000 == 0x01000000:
            return Insn(pc, w, 4, False, "swpb" if w & 0x00400000 else "swp", cond,
                        rn=rd, rd=rn, rm=rm)
        return _undef(w, pc, cond)            # ldrex/strex... (v6)
    p, u, i, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    if not p and wb:                          # ldrht/strht... (v6T2)
        return _undef(w, pc, cond)
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    if ld:
        op = (None, "ldrh", "ldrsb", "ldrsh")[sh]
    else:
        op = (None, "strh", "ldrd", "strd")[sh]
        if sh >= 2 and (arch < 5 or rd == 15):    # other odd rd: unpredictable
            return _undef(w, pc, cond)
    ins = Insn(pc, w, 4, False, op, cond, rd=rd, rn=rn, pre=bool(p), add=bool(u),
               writeback=bool(wb or not p))
    if sh >= 2 and not ld:
        ins.rd2 = rd + 1
    if i:
        ins.imm = ((w >> 4) & 0xF0) | (w & 15)
        if rn == 15 and p and not wb and op in LOADS:
            ins.target = (pc + 8 + (ins.imm if u else -ins.imm)) & 0xFFFFFFFF
    else:
        ins.rm = w & 15
    return ins


def _arm_ls(w, pc, cond, reg_form):
    """LDR/STR/LDRB/STRB (and the T variants: post-indexed with W set)."""
    if reg_form and w & 0x10:
        return _undef(w, pc, cond)            # media instructions (v6), udf
    p, u, b, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    user = bool(not p and wb)
    op = ("ldr" if ld else "str") + ("b" if b else "") + ("t" if user else "")
    ins = Insn(pc, w, 4, False, op, cond, rd=rd, rn=rn, pre=bool(p), add=bool(u),
               writeback=bool(wb or not p), user=user)
    if reg_form:
        ins.rm = w & 15
        ins.shift, ins.shift_imm = _imm_shift(w)
    else:
        ins.imm = w & 0xFFF
        if rn == 15 and p and not wb and ld:
            ins.target = (pc + 8 + (ins.imm if u else -ins.imm)) & 0xFFFFFFFF
    if ld and rd == 15 and op == "ldr":
        ins.exchange = "dynamic"              # cleared for ARMv4 in decode_arm
    return ins


def _arm_ldm(w, pc, cond):
    reglist = w & 0xFFFF
    if not reglist:
        return _undef(w, pc, cond)
    p, u, s, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    ins = Insn(pc, w, 4, False, "ldm" if ld else "stm", cond, rn=(w >> 16) & 15,
               reglist=reglist, mode=("da", "ia", "db", "ib")[p * 2 + u],
               writeback=bool(wb), user=bool(s))
    if ld and reglist & 0x8000 and not s:
        ins.exchange = "dynamic"
    return ins


def _arm_cop(w, pc, cond, two=False):
    """CDP/MCR/MRC (bits 27-24 = 1110); `two` for the unconditional
    CDP2/MCR2/MRC2."""
    sfx = "2" if two else ""
    kw = dict(crn=(w >> 16) & 15, cp=(w >> 8) & 15, op2=(w >> 5) & 7, crm=w & 15)
    if w & 0x10:
        return Insn(pc, w, 4, False, ("mrc" if w & 0x00100000 else "mcr") + sfx, cond,
                    op1=(w >> 21) & 7, rd=(w >> 12) & 15, **kw)
    return Insn(pc, w, 4, False, "cdp" + sfx, cond, op1=(w >> 20) & 15, crd=(w >> 12) & 15, **kw)


def _arm_ldc(w, pc, cond, arch, two=False):
    """LDC/STC (bits 27-25 = 110), MCRR/MRRC."""
    if w & 0x0FE00000 == 0x0C400000:
        if arch < 5 or two:                   # mcrr2/mrrc2 are v6
            return _undef(w, pc, cond)
        return Insn(pc, w, 4, False, "mrrc" if w & 0x00100000 else "mcrr", cond,
                    rn=(w >> 16) & 15, rd=(w >> 12) & 15, cp=(w >> 8) & 15,
                    op1=(w >> 4) & 15, crm=w & 15)
    p, u, n, wb, ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    if not p and not wb and not u:
        return _undef(w, pc, cond)
    ins = Insn(pc, w, 4, False, ("ldc" if ld else "stc") + ("2" if two else ""), cond,
               rn=(w >> 16) & 15, crd=(w >> 12) & 15, cp=(w >> 8) & 15, long=bool(n),
               pre=bool(p), add=bool(u), writeback=bool(wb))
    if not p and not wb:                      # unindexed: [rn], {option}
        ins.mode, ins.imm = "option", w & 0xFF
    else:
        ins.imm = (w & 0xFF) * 4
    return ins


def _arm_nv(w, pc, arch):
    """cond = 1111: BLX(imm), PLD and the coprocessor "2" forms (ARMv5)."""
    if arch < 5:
        return _undef(w, pc, 15)
    if w & 0x0E000000 == 0x0A000000:
        off = (_sext(w & 0xFFFFFF, 24) << 2) | ((w >> 23) & 2)
        return Insn(pc, w, 4, False, "blx", 14, target=(pc + 8 + off) & 0xFFFFFFFF,
                    exchange="thumb")
    if w & 0x0D70F000 == 0x0550F000:
        ins = Insn(pc, w, 4, False, "pld", 14, rn=(w >> 16) & 15, pre=True,
                   add=bool(w & 0x00800000), writeback=False)
        if w & 0x02000000:
            if w & 0x10:
                return _undef(w, pc, 15)
            ins.rm = w & 15
            ins.shift, ins.shift_imm = _imm_shift(w)
        else:
            ins.imm = w & 0xFFF
        return ins
    if w & 0x0F000000 == 0x0E000000:
        return _arm_cop(w, pc, 14, True)
    if w & 0x0E000000 == 0x0C000000:
        ins = _arm_ldc(w, pc, 15, arch, True)
        if ins.op != "undefined":
            ins.cond = 14
        return ins
    return _undef(w, pc, 15)                  # cps, srs, rfe, setend, NEON... (v6+)


def decode_arm(word, pc, arch=5):
    """Decode one ARM instruction at `pc`. arch=4 for the ARM7 (ARMv4T)."""
    w = word & 0xFFFFFFFF
    cond = w >> 28
    if cond == 15:
        return _arm_nv(w, pc, arch)
    k = (w >> 25) & 7
    if k == 0:
        if w & 0x90 == 0x90:
            return _arm_mul_extra(w, pc, cond, arch)
        if w & 0x01900000 == 0x01000000:
            return _arm_misc(w, pc, cond, arch)
        return _arm_dp(w, pc, cond, False)
    if k == 1:
        if w & 0x01900000 == 0x01000000:      # opcode 10xx, S=0
            if not w & 0x00200000:            # movw/movt (v6T2)
                return _undef(w, pc, cond)
            mask = (w >> 16) & 15
            if w & 0xF000 != 0xF000:          # bits 15-12 should be one
                return _undef(w, pc, cond)
            if not mask and not w & 0x00400000:   # msr cpsr, no field: a nop on v5
                return Insn(pc, w, 4, False, "nop", cond)   # (hints from v6K on)
            rot = ((w >> 8) & 15) * 2
            return Insn(pc, w, 4, False, "msr", cond, spsr=bool(w & 0x00400000), mask=mask,
                        imm=_ror(w & 0xFF, rot), rot=rot)
        return _arm_dp(w, pc, cond, True)
    if k == 2 or k == 3:
        ins = _arm_ls(w, pc, cond, k == 3)
        if arch < 5:
            ins.exchange = None
        return ins
    if k == 4:
        ins = _arm_ldm(w, pc, cond)
        if arch < 5:
            ins.exchange = None
        return ins
    if k == 5:
        return Insn(pc, w, 4, False, "bl" if w & 0x01000000 else "b", cond,
                    target=(pc + 8 + (_sext(w & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF)
    if k == 6:
        return _arm_ldc(w, pc, cond, arch)
    if w & 0x01000000:
        return Insn(pc, w, 4, False, "swi", cond, imm=w & 0xFFFFFF)
    return _arm_cop(w, pc, cond)


# ---------------------------------------------------------------- Thumb

_THUMB_ALU = ("and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror",
              "tst", "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn")


def _is_bl_suffix(h, arch):
    return h is not None and (h & 0xF800 == 0xF800 or (arch >= 5 and h & 0xF801 == 0xE800))


def decode_thumb(hw, pc, next_hw=None, arch=5):
    """Decode one Thumb instruction at `pc`. `next_hw` (the following
    halfword) lets a BL/BLX prefix decode as the full 4-byte call."""
    h = hw & 0xFFFF
    top = h >> 11
    lo3, mid3 = h & 7, (h >> 3) & 7
    T = True
    if top < 3:                               # lsl/lsr/asr rd, rm, #imm5
        amount = (h >> 6) & 31
        ins = Insn(pc, h, 2, T, "mov", s=True, rd=lo3, rm=mid3)
        if top == 0:
            if amount:
                ins.shift, ins.shift_imm = "lsl", amount
        else:
            ins.shift, ins.shift_imm = SHIFTS[top], amount or 32
        return ins
    if top == 3:                              # add/sub rd, rn, rm|#imm3
        ins = Insn(pc, h, 2, T, "sub" if h & 0x200 else "add", s=True, rd=lo3, rn=mid3)
        if h & 0x400:
            ins.imm = (h >> 6) & 7
        else:
            ins.rm = (h >> 6) & 7
        return ins
    if top < 8:                               # mov/cmp/add/sub rd, #imm8
        op = ("mov", "cmp", "add", "sub")[top - 4]
        rd = (h >> 8) & 7
        ins = Insn(pc, h, 2, T, op, s=True, imm=h & 0xFF)
        if op != "cmp":
            ins.rd = rd
        if op != "mov":
            ins.rn = rd
        return ins
    if top == 8:
        if not h & 0x400:                     # ALU rd, rs
            alu = _THUMB_ALU[(h >> 6) & 15]
            if alu in ("lsl", "lsr", "asr", "ror"):
                return Insn(pc, h, 2, T, "mov", s=True, rd=lo3, rm=lo3, shift=alu, rs=mid3)
            if alu == "neg":
                return Insn(pc, h, 2, T, "rsb", s=True, rd=lo3, rn=mid3, imm=0)
            if alu == "mul":
                return Insn(pc, h, 2, T, "mul", s=True, rd=lo3, rm=mid3, rs=lo3)
            if alu in COMPARES:
                return Insn(pc, h, 2, T, alu, s=True, rn=lo3, rm=mid3)
            if alu == "mvn":
                return Insn(pc, h, 2, T, "mvn", s=True, rd=lo3, rm=mid3)
            return Insn(pc, h, 2, T, alu, s=True, rd=lo3, rn=lo3, rm=mid3)
        op = (h >> 8) & 3                     # high registers, bx/blx
        rd, rm = lo3 | ((h >> 4) & 8), (h >> 3) & 15
        if op == 0:
            return Insn(pc, h, 2, T, "add", rd=rd, rn=rd, rm=rm)
        if op == 1:
            return Insn(pc, h, 2, T, "cmp", s=True, rn=rd, rm=rm)
        if op == 2:
            return Insn(pc, h, 2, T, "mov", rd=rd, rm=rm)
        if h & 0x80:
            if arch < 5:
                return _undef(h, pc, 14, T, 2)
            return Insn(pc, h, 2, T, "blx", rm=rm, exchange="dynamic")
        return Insn(pc, h, 2, T, "bx", rm=rm, exchange="dynamic")
    if top == 9:                              # ldr rd, [pc, #imm8*4]
        imm = (h & 0xFF) * 4
        return Insn(pc, h, 2, T, "ldr", rd=(h >> 8) & 7, rn=15, imm=imm, pre=True, add=True,
                    writeback=False, target=((pc + 4) & ~3) + imm & 0xFFFFFFFF)
    if top < 12:                              # register offset
        op = ("str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh")[(h >> 9) & 7]
        return Insn(pc, h, 2, T, op, rd=lo3, rn=mid3, rm=(h >> 6) & 7, pre=True, add=True,
                    writeback=False)
    if top < 18:                              # immediate offset (imm5 scaled)
        op, scale = (("str", 4), ("ldr", 4), ("strb", 1), ("ldrb", 1),
                     ("strh", 2), ("ldrh", 2))[top - 12]
        return Insn(pc, h, 2, T, op, rd=lo3, rn=mid3, imm=((h >> 6) & 31) * scale,
                    pre=True, add=True, writeback=False)
    if top < 20:                              # str/ldr rd, [sp, #imm8*4]
        return Insn(pc, h, 2, T, "ldr" if top & 1 else "str", rd=(h >> 8) & 7, rn=13,
                    imm=(h & 0xFF) * 4, pre=True, add=True, writeback=False)
    if top < 22:                              # add rd, pc|sp, #imm8*4
        imm = (h & 0xFF) * 4
        ins = Insn(pc, h, 2, T, "add", rd=(h >> 8) & 7, rn=13 if top & 1 else 15, imm=imm)
        if not top & 1:
            ins.target = ((pc + 4) & ~3) + imm & 0xFFFFFFFF
        return ins
    if top < 24:                              # miscellaneous
        if h & 0xFF00 == 0xB000:              # add/sub sp, #imm7*4
            return Insn(pc, h, 2, T, "sub" if h & 0x80 else "add", rd=13, rn=13,
                        imm=(h & 0x7F) * 4)
        if h & 0xF600 == 0xB400:              # push {list, lr} / pop {list, pc}
            pop = bool(h & 0x800)
            reglist = (h & 0xFF) | ((h & 0x100) << (7 if pop else 6))
            if not reglist:
                return _undef(h, pc, 14, T, 2)
            ins = Insn(pc, h, 2, T, "ldm" if pop else "stm", rn=13, reglist=reglist,
                       mode="ia" if pop else "db", writeback=True, user=False)
            if pop and reglist & 0x8000 and arch >= 5:
                ins.exchange = "dynamic"
            return ins
        if h & 0xFF00 == 0xBE00 and arch >= 5:
            return Insn(pc, h, 2, T, "bkpt", imm=h & 0xFF)
        return _undef(h, pc, 14, T, 2)        # cbz, sxth, rev, cps, it... (v6+)
    if top < 26:                              # stmia/ldmia rn!, {list}
        rn, reglist = (h >> 8) & 7, h & 0xFF
        if not reglist:
            return _undef(h, pc, 14, T, 2)
        ld = bool(top & 1)
        return Insn(pc, h, 2, T, "ldm" if ld else "stm", rn=rn, reglist=reglist, mode="ia",
                    writeback=not (ld and reglist >> rn & 1), user=False)
    if top < 28:
        cond = (h >> 8) & 15
        if cond == 15:
            return Insn(pc, h, 2, T, "swi", imm=h & 0xFF)
        if cond == 14:
            return _undef(h, pc, 14, T, 2)
        return Insn(pc, h, 2, T, "b", cond, target=(pc + 4 + (_sext(h & 0xFF, 8) << 1)) & 0xFFFFFFFF)
    if top == 28:
        return Insn(pc, h, 2, T, "b", target=(pc + 4 + (_sext(h & 0x7FF, 11) << 1)) & 0xFFFFFFFF)
    if top == 30:                             # BL/BLX prefix
        hi = _sext(h & 0x7FF, 11) << 12
        if _is_bl_suffix(next_hw, arch):
            n = next_hw & 0xFFFF
            t = (pc + 4 + hi + ((n & 0x7FF) << 1)) & 0xFFFFFFFF
            if n & 0x1000:
                return Insn(pc, (h << 16) | n, 4, T, "bl", target=t)
            return Insn(pc, (h << 16) | n, 4, T, "blx", target=t & ~3, exchange="arm")
        return Insn(pc, h, 2, T, "bl_hi", imm=hi)   # lr = pc + 4 + imm
    if top == 31:                             # lone BL suffix: pc = lr + imm
        return Insn(pc, h, 2, T, "bl_lo", imm=(h & 0x7FF) << 1)
    if arch >= 5 and not h & 1:               # lone BLX suffix (v5T)
        return Insn(pc, h, 2, T, "blx_lo", imm=(h & 0x7FF) << 1, exchange="arm")
    return _undef(h, pc, 14, T, 2)


def decode(data_or_image, pc, thumb=False, arch=5):
    """Decode the instruction at `pc`, from an Image (read at address pc) or
    from bytes holding the instruction's encoding at offset 0."""
    if isinstance(data_or_image, Image):
        return data_or_image.insn(pc, thumb)
    d = data_or_image
    if thumb:
        nxt = struct.unpack_from("<H", d, 2)[0] if len(d) >= 4 else None
        return decode_thumb(struct.unpack_from("<H", d, 0)[0], pc, nxt, arch)
    return decode_arm(struct.unpack_from("<I", d, 0)[0], pc, arch)


# ---------------------------------------------------------------- text


def _i(v):
    """Immediate as capstone prints it: decimal up to 9, hex beyond."""
    if v < 0:
        return "#-%d" % -v if v > -10 else "#-0x%x" % -v
    return "#%d" % v if v < 10 else "#0x%x" % v


def _sh(ins):
    if ins.shift is None:
        return ""
    if ins.shift == "rrx":
        return ", rrx"
    if ins.rs is not None:
        return ", %s %s" % (ins.shift, REGS[ins.rs])
    return ", %s %s" % (ins.shift, _i(ins.shift_imm))


def _regs(mask):
    return "{" + ", ".join(REGS[i] for i in range(16) if mask >> i & 1) + "}"


def _mem(ins):
    """Addressing-mode text: [rn, #off]!, [rn], #off, [rn, -rm, lsl #2]..."""
    rn = REGS[ins.rn]
    if ins.rm is None:
        off = ins.imm or 0
        o = _i(off if ins.add else -off) if off or ins.add else "#-0"
        if not ins.pre:
            s = "[%s], %s" % (rn, o)
        elif off == 0 and ins.add and not ins.writeback:
            s = "[%s]" % rn
        else:
            s = "[%s, %s]%s" % (rn, o, "!" if ins.writeback else "")
    else:
        o = ("" if ins.add else "-") + REGS[ins.rm] + _sh(ins)
        s = "[%s, %s]%s" % (rn, o, "!" if ins.writeback else "") if ins.pre else "[%s], %s" % (rn, o)
    if ins.target is not None:
        s += "  ; 0x%08X" % ins.target
    return s


def _psr(ins):
    if not ins.mask:
        return "spsr" if ins.spsr else "cpsr"
    return ("spsr_" if ins.spsr else "cpsr_") + "".join(
        c for c, b in (("f", 8), ("s", 4), ("x", 2), ("c", 1)) if ins.mask & b)


def _thumb_text(ins):
    """The Thumb forms whose capstone syntax differs from the ARM one."""
    h, op = ins.raw, ins.op
    if ins.size == 4:
        return None
    if op == "bl_hi":
        return "bl_hi %s" % _i(ins.imm)
    if op in ("bl_lo", "blx_lo"):
        return "%s %s" % (op, _i(ins.imm))
    top = h >> 11
    if 6 <= top <= 7:                         # adds/subs rd, #imm8
        return "%ss %s, %s" % (op, REGS[ins.rd], _i(ins.imm))
    if h >> 10 == 0x10:
        if op == "mov":                       # lsls rd, rs
            return "%ss %s, %s" % (ins.shift, REGS[ins.rd], REGS[ins.rs])
        if op in ("and", "eor", "adc", "sbc", "orr", "bic"):
            return "%ss %s, %s" % (op, REGS[ins.rd], REGS[ins.rm])
    if h >> 8 == 0x44:                        # add rd, rm (high registers)
        if ins.rm == 13:
            return "add %s, sp, %s" % (REGS[ins.rd], REGS[ins.rd])
        return "add %s, %s" % (REGS[ins.rd], REGS[ins.rm])
    if top == 9:                              # ldr rd, [pc, #imm] keeps #0
        return "ldr %s, [pc, %s]  ; 0x%08X" % (REGS[ins.rd], _i(ins.imm), ins.target)
    if top == 20:
        return "adr %s, %s  ; 0x%08X" % (REGS[ins.rd], _i(ins.imm), ins.target)
    if h >> 8 == 0xB0:
        return "%s sp, %s" % (op, _i(ins.imm))
    if h & 0xF600 == 0xB400:
        return "%s %s" % ("pop" if op == "ldm" else "push", _regs(ins.reglist))
    return None


def _text(ins):
    op, c = ins.op, CONDS[ins.cond]
    if op == "undefined":
        return ".hword 0x%04X" % ins.raw if ins.thumb else ".word 0x%08X" % ins.raw
    if ins.thumb:
        t = _thumb_text(ins)
        if t:
            return t
    R = REGS
    if op in DATA_OPS:
        s = "s" if ins.s and op not in COMPARES else ""
        op2 = _i(ins.imm) if ins.imm is not None else R[ins.rm] + _sh(ins)
        if op == "mov" and ins.shift is not None:     # UAL: lsl rd, rm, #n
            if ins.shift == "rrx":
                return "rrx%s%s %s, %s" % (s, c, R[ins.rd], R[ins.rm])
            amt = R[ins.rs] if ins.rs is not None else _i(ins.shift_imm)
            return "%s%s%s %s, %s, %s" % (ins.shift, s, c, R[ins.rd], R[ins.rm], amt)
        if op in COMPARES:
            return "%s%s %s, %s" % (op, c, R[ins.rn], op2)
        if op in ("mov", "mvn"):
            return "%s%s%s %s, %s" % (op, s, c, R[ins.rd], op2)
        t = "%s%s%s %s, %s, %s" % (op, s, c, R[ins.rd], R[ins.rn], op2)
        return t + ("  ; 0x%08X" % ins.target if ins.target is not None else "")
    if op in LOADS or op in STORES:
        if op == "ldr" and ins.rn == 13 and not ins.pre and ins.add and ins.imm == 4 \
                and not ins.thumb:            # ldr rX, [sp], #4 (capstone's alias)
            return "pop%s {%s}" % (c, R[ins.rd])
        if op in ("ldrd", "strd"):
            return "%s%s %s, %s, %s" % (op, c, R[ins.rd], R[ins.rd + 1], _mem(ins))
        return "%s%s %s, %s" % (op, c, R[ins.rd], _mem(ins))
    if op in ("ldm", "stm"):
        n = bin(ins.reglist).count("1")
        if ins.rn == 13 and ins.writeback and not ins.user and n > 1 and \
                (op, ins.mode) in (("stm", "db"), ("ldm", "ia")):
            return "%s%s %s" % ("push" if op == "stm" else "pop", c, _regs(ins.reglist))
        return "%s%s%s %s%s, %s%s" % (op, "" if ins.mode == "ia" else ins.mode, c, R[ins.rn],
                                      "!" if ins.writeback else "", _regs(ins.reglist),
                                      " ^" if ins.user else "")
    if op in ("b", "bl") or (op == "blx" and ins.target is not None):
        return "%s%s 0x%08X" % (op, c, ins.target)
    if op in ("bx", "blx"):
        return "%s%s %s" % (op, c, R[ins.rm])
    if op in _MULS:
        s = "s" if ins.s else ""
        t = "%s%s%s %s, %s, %s" % (op, s, c, R[ins.rd], R[ins.rm], R[ins.rs])
        return t + (", " + R[ins.rn] if op == "mla" else "")
    if op in _LONG_MULS:
        return "%s%s%s %s, %s, %s, %s" % (op, "s" if ins.s else "", c, R[ins.rd], R[ins.rd2],
                                          R[ins.rm], R[ins.rs])
    if op.startswith("smlal"):
        return "%s%s %s, %s, %s, %s" % (op, c, R[ins.rd], R[ins.rd2], R[ins.rm], R[ins.rs])
    if op.startswith("smla"):
        return "%s%s %s, %s, %s, %s" % (op, c, R[ins.rd], R[ins.rm], R[ins.rs], R[ins.rn])
    if op.startswith("smul"):
        return "%s%s %s, %s, %s" % (op, c, R[ins.rd], R[ins.rm], R[ins.rs])
    if op in ("qadd", "qsub", "qdadd", "qdsub"):
        return "%s%s %s, %s, %s" % (op, c, R[ins.rd], R[ins.rm], R[ins.rn])
    if op == "clz":
        return "clz%s %s, %s" % (c, R[ins.rd], R[ins.rm])
    if op == "mrs":
        return "mrs%s %s, %s" % (c, R[ins.rd], _psr(ins))
    if op == "msr":
        src = _i(ins.imm) if ins.imm is not None else R[ins.rm]
        return "msr%s %s, %s" % (c, _psr(ins), src)
    if op in ("swp", "swpb"):
        return "%s%s %s, %s, [%s]" % (op, c, R[ins.rd], R[ins.rm], R[ins.rn])
    if op == "swi":
        return "swi%s %s" % (c, _i(ins.imm))
    if op == "bkpt":
        return "bkpt %s" % _i(ins.imm)
    if op == "nop":
        return "nop" + c
    if op in ("mcr", "mrc", "mcr2", "mrc2"):
        rd = "apsr_nzcv" if op[:3] == "mrc" and ins.rd == 15 else R[ins.rd]
        return "%s%s p%d, #%d, %s, c%d, c%d, #%d" % (op, c, ins.cp, ins.op1, rd, ins.crn,
                                                     ins.crm, ins.op2)
    if op in ("cdp", "cdp2"):
        return "%s%s p%d, #%d, c%d, c%d, c%d, #%d" % (op, c, ins.cp, ins.op1, ins.crd, ins.crn,
                                                      ins.crm, ins.op2)
    if op in ("mcrr", "mrrc"):
        return "%s%s p%d, #%d, %s, %s, c%d" % (op, c, ins.cp, ins.op1, R[ins.rd], R[ins.rn],
                                               ins.crm)
    if op[:3] in ("ldc", "stc"):
        if ins.mode == "option":
            m = "[%s], {%s}" % (R[ins.rn], _i(ins.imm)[1:])
        else:
            m = _mem(ins)
        return "%s%s%s p%d, c%d, %s" % (op, "l" if ins.long else "", c, ins.cp, ins.crd, m)
    if op == "pld":
        return "pld " + _mem(ins)
    return op


# ---------------------------------------------------------------- images


class Image:
    """A flat binary loaded at `base` (the ARM9 or ARM7 binary, an overlay).
    arch=4 decodes for the ARM7TDMI."""

    def __init__(self, data, base, arch=5):
        self.data, self.base, self.arch = data, base, arch
        self.end = base + len(data)

    def contains(self, addr, size=1):
        return self.base <= addr and addr + size <= self.end

    def u8(self, addr):
        return self.data[addr - self.base]

    def u16(self, addr):
        return struct.unpack_from("<H", self.data, addr - self.base)[0]

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def insn(self, pc, thumb=False):
        if thumb:
            nxt = self.u16(pc + 2) if self.contains(pc + 2, 2) else None
            return decode_thumb(self.u16(pc), pc, nxt, self.arch)
        return decode_arm(self.u32(pc), pc, self.arch)

    def literal(self, ins):
        """Value a pc-relative load reads (zero/sign-extended to 32 bits,
        (lo, hi) for ldrd), or None if not one or outside the image."""
        n = ins.literal_size
        if n is None or not self.contains(ins.target, n):
            return None
        a = ins.target
        if n == 8:
            return self.u32(a), self.u32(a + 4)
        v = self.u32(a) if n == 4 else self.u16(a) if n == 2 else self.u8(a)
        if ins.op == "ldrsb":
            v = _sext(v, 8) & 0xFFFFFFFF
        elif ins.op == "ldrsh":
            v = _sext(v, 16) & 0xFFFFFFFF
        return v

    def disasm(self, pc, count, thumb=False):
        """`count` instructions from `pc` as text lines, literal values
        (and their hardware register names) in the comment."""
        out = []
        a = pc
        for _ in range(count):
            if not self.contains(a, 2 if thumb else 4):
                break
            ins = self.insn(a, thumb)
            s = repr(ins)
            v = self.literal(ins)
            if isinstance(v, int):
                s += " = 0x%08X" % v
                name = hw.name(v, 7 if self.arch < 5 else 9)
                if name:
                    s += " " + name
            out.append(s)
            a += ins.size
        return out

    def census(self, lo=None, hi=None, thumb=False):
        """Count of ops over [lo, hi) decoded in one mode."""
        a = self.base if lo is None else lo
        hi = self.end if hi is None else min(hi, self.end)
        step = 2 if thumb else 4
        a = (a + step - 1) & ~(step - 1)
        c = collections.Counter()
        while a + step <= hi:
            ins = self.insn(a, thumb)
            c[ins.op] += 1
            a += ins.size
        return c

    def literal_refs(self, lo, hi):
        """Every pc-relative word load (ARM ldr rX,[pc,#+-imm], Thumb
        ldr rX,[pc,#imm]) whose value lies in [lo, hi), as
        (pc, literal address, value, thumb).

        A raw binary does not say which areas are ARM and which Thumb, so
        both decodings are scanned (every word as ARM, every halfword as
        Thumb): data and code of the other mode yield some false hits.
        A literal read by real code usually shows up from one mode only.
        """
        out = []
        d, base = self.data, self.base
        b0 = (-base) & 3                      # first word-aligned offset
        for off in range(b0, len(d) - 3, 4):
            w = struct.unpack_from("<I", d, off)[0]
            if w & 0x0F7F0000 == 0x051F0000 and w >> 28 != 15:
                ins = decode_arm(w, base + off, self.arch)
                v = self.literal(ins)
                if v is not None and lo <= v < hi:
                    out.append((ins.pc, ins.target, v, False))
        for off in range(base & 1, len(d) - 1, 2):
            h = struct.unpack_from("<H", d, off)[0]
            if h & 0xF800 == 0x4800:
                ins = decode_thumb(h, base + off, None, self.arch)
                v = self.literal(ins)
                if v is not None and lo <= v < hi:
                    out.append((ins.pc, ins.target, v, True))
        out.sort()
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--base", default="02000000", help="load address (hex)")
    ap.add_argument("--offset", default="0", help="file offset of the binary (hex)")
    ap.add_argument("--size", help="size of the binary (hex, default: to end of file)")
    ap.add_argument("--at", help="disassemble from this address (hex; odd = Thumb)")
    ap.add_argument("--count", type=int, default=64)
    ap.add_argument("--thumb", action="store_true", help="decode as Thumb")
    ap.add_argument("--arm7", action="store_true", help="ARM7TDMI (ARMv4T) instead of ARMv5TE")
    ap.add_argument("--census", action="store_true", help="count ops over the image")
    ap.add_argument("--refs", help="LO[:HI] hex: literal loads of values in the range")
    a = ap.parse_args(argv)
    with open(a.file, "rb") as f:
        f.seek(int(a.offset, 16))
        data = f.read(int(a.size, 16)) if a.size else f.read()
    img = Image(data, int(a.base, 16), 4 if a.arm7 else 5)
    if a.at:
        at = int(a.at, 16)
        print("\n".join(img.disasm(at & ~1, a.count, a.thumb or bool(at & 1))))
    if a.census:
        for op, n in img.census(thumb=a.thumb).most_common():
            print("%-10s %d" % (op, n))
    if a.refs:
        lo, _, hi = a.refs.partition(":")
        lo = int(lo, 16)
        hi = int(hi, 16) if hi else lo + 1
        for pc, lit, v, thumb in img.literal_refs(lo, hi):
            ins = img.insn(pc, thumb)
            print("%08X  %s  %-36s = 0x%08X %s" % (pc, "T" if thumb else "A", ins.text, v,
                                                    hw.name(v, 7 if img.arch < 5 else 9) or ""))


if __name__ == "__main__":
    main()
