"""ARM and Thumb instructions -> C++, against ndskit/runtime/arm.h.

Body turns a discovered function (recomp.discover) into one C++ function,
`void f_XXXXXXXX(ArmContext& c)` for ARM code, `t_XXXXXXXX` for Thumb, with
the semantics of ndskit/armemu.py instruction for instruction:

* straight-line order by address, a label wherever something jumps, a
  `goto` where the code continues elsewhere (past a literal pool);
* the condition of each instruction as an `if`; the flags computed only
  where they are read: a backward liveness pass over the function's flow
  (N, Z, C, V each), every flag live at its exits and before its calls,
  so a flag is stored only if some path reads it before it is set again;
* the shifter's carry only where a logical instruction with S sets C;
* the PC as a value: reads of r15 are constants (+8 in ARM, +4 in Thumb,
  the word-aligned pc for Thumb's adr and literal loads); literal loads
  are constants read from the image, unless the pool slot is data some
  literal points at;
* `b` inside the function as a `goto`, to another entry as a tail call;
  `bl`/`blx` as C++ calls with lr set first, the return checked
  (ARM_RET: the callee's return must come back to lr);
* everything that writes pc from a value takes what discovery decided at
  that address (Function.sites): a return (`c.pc = target; return;`), a
  call through a register (dispatched over the known entries: compared
  with the target analysis expects, else arm_call, with the mode in bit 0
  of the address), a switch (a C++ `switch` over its targets, the rest to
  arm_jump), or a jump (to a known entry as a tail call; otherwise
  arm_jump, which calls an entry or returns to an address that is none).
  After a jump, and at a return in a function that sets lr to its own
  addresses for hand-written dispatch, control that comes back to one of
  those addresses goes on there;
* interworking: bx, blx and (on the ARMv5) loads into pc take the mode from
  bit 0; mov/add pc keep it;
* processor modes, the SPSR and exception returns (`subs pc, lr, #4`,
  `ldm ..., {..., pc}^`), CP15, SWI and undefined instructions as runtime
  calls (arm_set_cpsr, arm_cp15_*, arm_swi, arm_undefined...);
* safe points (ARM_POLL) at backward jumps, before calls and after an
  msr that can unmask interrupts.

Every multi-statement instruction is its own block, so a `goto` never
crosses an initialisation.
"""
from .. import arm

M32 = 0xFFFFFFFF
FN, FZ, FC, FV = 1, 2, 4, 8
ALL = 15

COND_USE = (FZ, FZ, FC, FC, FN, FN, FV, FV, FC | FZ, FC | FZ, FN | FV, FN | FV,
            FZ | FN | FV, FZ | FN | FV, 0, 0)
COND_EXPR = ("c.z", "!c.z", "c.c", "!c.c", "c.n", "!c.n", "c.v", "!c.v",
             "c.c && !c.z", "!c.c || c.z", "c.n == c.v", "c.n != c.v",
             "!c.z && c.n == c.v", "c.z || c.n != c.v", "true", "true")
LOGIC = frozenset(("and", "eor", "orr", "bic", "mov", "mvn", "tst", "teq"))
ARITH = frozenset(("add", "adc", "sub", "sbc", "rsb", "rsc", "cmp", "cmn"))


class Unsupported(Exception):
    pass


def fname(addr, thumb=False):
    return ("t_%08X" if thumb else "f_%08X") % addr


def _h(v):
    v &= M32
    return "0x%08Xu" % v if v > 0xFFFF else "0x%Xu" % v


def _r(n):
    return "c.r[%d]" % n


def _pcv(ins, extra=0):
    """The value an instruction reads from r15."""
    return (ins.pc + (4 if ins.thumb else 8 + extra)) & M32


def _reg(ins, n, extra=0):
    return _h(_pcv(ins, extra)) if n == 15 else _r(n)


# ---------------------------------------------------------------- flags of an instruction


def _shift_sets_c(ins):
    """Whether the shifter's carry-out replaces C (a logical op with S)."""
    if ins.imm is not None:
        return bool(ins.rot)
    return ins.shift is not None


def flags_rw(ins):
    """(flags read, flags written when it runs) of a non-transfer instruction."""
    op = ins.op
    use = COND_USE[ins.cond]
    if ins.shift == "rrx" or op in ("adc", "sbc", "rsc"):
        use |= FC
    if op in LOGIC or op in ARITH:
        if ins.s or op in arm.COMPARES:
            if op in ARITH:
                return use, ALL
            if ins.rs is not None:          # by a register: an amount of 0 keeps C
                use |= FC
            return use, FN | FZ | (FC if _shift_sets_c(ins) else 0)
        return use, 0
    if op in ("mul", "mla", "umull", "umlal", "smull", "smlal"):
        return use, (FN | FZ) if ins.s else 0
    if op == "mrs":
        return use | (0 if ins.spsr else ALL), 0
    if op == "msr":
        return use, ALL if not ins.spsr and ins.mask & 8 else 0
    if op == "mrc" and ins.rd == 15:
        return use, ALL
    if op == "swi":
        return use | ALL, 0
    return use, 0


# ---------------------------------------------------------------- one function


class Body:
    """One discovered function as C++.

    `prog`: the discover.Program; `entries`: {key: thumb} of every entry of
    this program (key = address | thumb); `volatile`: literal-pool slots not
    to fold into constants; `arch`: 5 (ARM9) or 4 (ARM7). After emit():
    `sites` counts the calls and jumps through registers by how they were
    resolved, `unknown` lists static targets that are not entries.
    """

    def __init__(self, prog, f, entries, volatile=frozenset(), arch=5, comments=True):
        self.p, self.f, self.mem = prog, f, prog.mem
        self.thumb, self.T = f.thumb, int(f.thumb)
        self.entries, self.volatile, self.arch, self.comments = entries, volatile, arch, comments
        self.ops = {a: self.mem.insn(a, f.thumb) for a in f.code}
        self.sites = {}
        self.unknown = []
        self.landings = self._landings()
        self.live = self._liveness()

    # ---- analysis -----------------------------------------------------
    def _landings(self):
        """Addresses of this function that it puts in lr itself for its own
        hand-written dispatch (not the return address of a call site): after a
        jump, or at a return, control arriving there goes on there."""
        code, sites = self.f.code, self.f.sites
        rets = {s[2] for s in sites.values() if s[0] == "call" and s[2] is not None}
        out = {s[2] & ~1 for s in sites.values() if s[0] == "switch" and s[2] is not None}
        for a, ins in self.ops.items():
            if ins.rd != 14 or ins.thumb:
                continue
            v = None
            if ins.op == "mov" and ins.rm == 15 and ins.imm is None and ins.shift is None:
                v = ins.pc + 8
            elif ins.op in ("add", "sub") and ins.rn == 15 and ins.target is not None:
                v = ins.target
            if v is not None and v not in rets and v in code:
                out.add(v)
        return out & code

    def _succ(self, a):
        """(local successors, exits) of the instruction at a, for liveness."""
        ins = self.ops[a]
        nxt = a + ins.size
        code = self.f.code
        cond = ins.cond != 14
        out = []
        ext = False
        op = ins.op
        if op == "b":
            if ins.target in code:
                out.append(ins.target)
            else:
                ext = True
            if cond:
                out.append(nxt)
        elif op in ("bl", "blx") and ins.target is not None:
            out.append(nxt)
        elif (op == "blx" or a in self.f.sites) and not self._exception_return(ins):
            kind, target, ret = self.f.sites.get(a, ("call", None, nxt | self.T))
            if kind == "call":                  # back at lr: here, further on, or another entry
                out.append((ret & ~1) if ret is not None else nxt)
            elif kind == "switch":
                out += [t for t in self.p.switches.get(a, ()) if t in code]
                if ret is not None and (ret & ~1) in code:
                    out.append(ret & ~1)
                ext = True
                out += sorted(self.landings)
            else:                               # return or jump: may come back to a landing
                if kind == "jump" and target is not None and (target & ~1) in code:
                    out.append(target & ~1)
                ext = True
                out += sorted(self.landings)
            if cond:
                out.append(nxt)
        elif ins.writes_pc:
            ext = True
            if cond:
                out.append(nxt)
        else:
            out.append(nxt)
        return [s for s in out if s in code], ext or any(s not in code for s in out)

    def _liveness(self):
        """{address: flags live after the instruction}."""
        code = sorted(self.f.code)
        succ, uses, defs = {}, {}, {}
        for a in code:
            ins = self.ops[a]
            succ[a] = self._succ(a)
            if ins.writes_pc or ins.is_call or (ins.op == "blx"):
                u, d = flags_rw(ins)[0], 0
                if ins.is_call or a in self.f.sites and self.f.sites[a][0] == "call":
                    u, d = ALL, ALL         # a call: the callee may read them, then sets them
            else:
                u, d = flags_rw(ins)
            uses[a] = u
            defs[a] = d if ins.cond == 14 else 0
        live_out = {a: 0 for a in code}
        live_in = {a: 0 for a in code}
        changed = True
        while changed:
            changed = False
            for a in reversed(code):
                ss, ext = succ[a]
                o = ALL if ext else 0
                for s in ss:
                    o |= live_in[s]
                i = uses[a] | (o & ~defs[a])
                if o != live_out[a] or i != live_in[a]:
                    live_out[a], live_in[a] = o, i
                    changed = True
        return live_out

    # ---- expressions ---------------------------------------------------
    def _literal(self, ins):
        """The constant a pc-relative load reads, or None to read memory."""
        size = ins.literal_size
        if not size or ins.target in self.volatile or ins.op == "ldrd":
            return None
        s = self.mem.seg(ins.target)
        if s is None or ins.target + size > s.end:
            return None
        v = int.from_bytes(s.data[ins.target - s.base:ins.target - s.base + size], "little")
        if ins.op == "ldrsb":
            v = (v - 0x100 if v & 0x80 else v) & M32
        elif ins.op == "ldrsh":
            v = (v - 0x10000 if v & 0x8000 else v) & M32
        elif ins.op == "ldr" and ins.target & 3:
            v = ((v >> (8 * (ins.target & 3))) | (v << (32 - 8 * (ins.target & 3)))) & M32
        return v

    def _base(self, ins):
        """Rn as an operand (adr and Thumb literal loads: the aligned pc)."""
        if ins.rn == 15:
            if ins.thumb:
                return _h((ins.pc + 4) & ~3)
            return _h(_pcv(ins, 4 if ins.rs is not None else 0))
        return _r(ins.rn)

    def _op2(self, ins, carry):
        """(C++ for the second operand, C++ for the shifter's carry or None
        when C stays, preamble statements). With `carry`, the carry is wanted."""
        if ins.imm is not None:
            v = ins.imm & M32
            return _h(v), (str(v >> 31) if ins.rot else None), ""
        extra = 0 if ins.thumb else 4
        if ins.rs is not None:
            m = _reg(ins, ins.rm, extra)
            n = "(%s & 0xFFu)" % _r(ins.rs)
            if carry:
                return "arm_%s_c(%s, %s, sc)" % (ins.shift, m, n), "sc", "uint32_t sc = c.c; "
            return "arm_%s(%s, %s)" % (ins.shift, m, n), None, ""
        m = _reg(ins, ins.rm)
        return _shift_imm(ins.shift, ins.shift_imm, m), _shift_imm_c(ins.shift, ins.shift_imm, m), ""

    # ---- non-transfer instructions -------------------------------------
    def _dp(self, ins, live):
        op, rd = ins.op, ins.rd
        sets = ins.s or op in arm.COMPARES
        want = live if sets else 0
        if op in LOGIC:
            wc = bool(want & FC) and _shift_sets_c(ins)
            b, cb, pre = self._op2(ins, wc)
            a = self._base(ins) if op not in ("mov", "mvn") else None
            r = {"and": "a & b", "eor": "a ^ b", "orr": "a | b", "bic": "a & ~b", "mov": "b",
                 "mvn": "~b", "tst": "a & b", "teq": "a ^ b"}[op]
            fl = []
            if want & FN:
                fl.append("c.n = r >> 31;")
            if want & FZ:
                fl.append("c.z = r == 0;")
            if wc and cb is not None:
                fl.append("c.c = %s;" % ("sc" if cb == "sc" else "cf"))
            if not fl and op not in arm.COMPARES and not pre:
                if op in ("mov", "mvn"):
                    e = b if op == "mov" else "~(%s)" % b
                else:
                    e = {"and": "%s & %s", "eor": "%s ^ %s", "orr": "%s | %s", "bic": "%s & ~%s"}[op] \
                        % (a, "(%s)" % b)
                return "%s = %s;" % (_r(rd), e)
            if not fl:
                return ";"
            parts = [pre]
            if a is not None:
                parts.append("const uint32_t a = %s; " % a)
            parts.append("const uint32_t b = %s; " % b)
            if wc and cb not in (None, "sc"):
                parts.append("const uint32_t cf = %s; " % cb)
            parts.append("const uint32_t r = %s; " % r)
            if op not in arm.COMPARES:
                parts.append("%s = r; " % _r(rd))
            return "{ %s%s }" % ("".join(parts), " ".join(fl))
        # arithmetic
        b, _, _ = self._op2(ins, False)           # (no carry wanted: no preamble)
        a = self._base(ins)
        if op in ("rsb", "rsc"):
            a, b = b, a
        fl = []
        carry_in = op in ("adc", "sbc", "rsc")
        if not want:
            if op in ("cmp", "cmn"):
                return ";"
            e = {"add": "%s + %s", "adc": "%s + %s + c.c", "sub": "%s - %s", "sbc": "%s - %s - !c.c",
                 "rsb": "%s - %s", "rsc": "%s - %s - !c.c"}[op] % (a, "(%s)" % b if " " in b else b)
            return "%s = %s;" % (_r(rd), e)
        add = op in ("add", "adc", "cmn")
        parts = ["const uint32_t a = %s, b = %s; " % (a, b)]
        if carry_in:
            if add:
                parts.append("const uint64_t s = (uint64_t)a + b + c.c; const uint32_t r = (uint32_t)s; ")
            else:
                parts.append("const uint64_t s = (uint64_t)a + (uint32_t)~b + c.c; const uint32_t r = (uint32_t)s; ")
        else:
            parts.append("const uint32_t r = a %s b; " % ("+" if add else "-"))
        if want & FN:
            fl.append("c.n = r >> 31;")
        if want & FZ:
            fl.append("c.z = r == 0;")
        if want & FC:
            if carry_in:
                fl.append("c.c = (uint32_t)(s >> 32);")
            else:
                fl.append("c.c = %s;" % ("r < a" if add else "a >= b"))
        if want & FV:
            fl.append("c.v = (%s) >> 31;" % ("~(a ^ b) & (a ^ r)" if add else "(a ^ b) & (a ^ r)"))
        if op not in ("cmp", "cmn"):
            parts.append("%s = r; " % _r(rd))
        return "{ %s%s }" % ("".join(parts), " ".join(fl))

    def _mul(self, ins, live):
        op, R = ins.op, _r
        want = live & (FN | FZ) if ins.s else 0
        if op in ("mul", "mla"):
            e = "%s * %s" % (R(ins.rm), R(ins.rs))
            if op == "mla":
                e += " + " + R(ins.rn)
            if not want:
                return "%s = %s;" % (R(ins.rd), e)
            fl = ("c.n = r >> 31; " if want & FN else "") + ("c.z = r == 0; " if want & FZ else "")
            return "{ const uint32_t r = %s; %s = r; %s}" % (e, R(ins.rd), fl)
        if op in ("umull", "umlal", "smull", "smlal"):
            if op[0] == "u":
                e = "(uint64_t)%s * %s" % (R(ins.rm), R(ins.rs))
            else:
                e = "(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s)" % (R(ins.rm), R(ins.rs))
            if op.endswith("lal"):
                e += " + ((uint64_t)%s << 32 | %s)" % (R(ins.rd2), R(ins.rd))
            fl = ("c.n = (uint32_t)(p >> 63); " if want & FN else "") + ("c.z = p == 0; " if want & FZ else "")
            return "{ const uint64_t p = %s; %s = (uint32_t)p; %s = (uint32_t)(p >> 32); %s}" % (
                e, R(ins.rd), R(ins.rd2), fl)
        # the DSP multiplies
        def half(n, top):
            return "(int32_t)(int16_t)(%s >> 16)" % R(n) if top else "(int32_t)(int16_t)%s" % R(n)
        if op.startswith("smlal"):
            return ("{ const int64_t p = (int64_t)%s * %s; const uint64_t s = ((uint64_t)%s << 32 | %s) + (uint64_t)p; "
                    "%s = (uint32_t)s; %s = (uint32_t)(s >> 32); }"
                    % (half(ins.rm, op[5] == "t"), half(ins.rs, op[6] == "t"), R(ins.rd2), R(ins.rd),
                       R(ins.rd), R(ins.rd2)))
        if op[4] == "w":
            p = "(int32_t)(((int64_t)(int32_t)%s * %s) >> 16)" % (R(ins.rm), half(ins.rs, op[5] == "t"))
        else:
            p = "(%s * %s)" % (half(ins.rm, op[4] == "t"), half(ins.rs, op[5] == "t"))
        if op.startswith("smul"):
            return "%s = (uint32_t)%s;" % (R(ins.rd), p)
        return ("{ const int64_t s = (int64_t)%s + (int32_t)%s; if (s != (int32_t)s) c.q = 1; %s = (uint32_t)s; }"
                % (p, R(ins.rn), R(ins.rd)))

    def _addr(self, ins):
        """(preamble, address expression, writeback statement) of a single load/store."""
        base = self._base(ins)
        if ins.rm is not None:
            off = _shift_imm(ins.shift, ins.shift_imm, _reg(ins, ins.rm))
        else:
            off = _h(ins.imm or 0) if ins.imm else None
        if off is None:
            moved = base
        else:
            moved = "%s %s %s" % (base, "+" if ins.add else "-", off)
        wb = ins.writeback and ins.rn != 15
        if not wb:
            return "", moved if ins.pre else base, ""
        if ins.pre:
            return "const uint32_t a = %s; " % moved, "a", "%s = a; " % _r(ins.rn)
        return ("const uint32_t a = %s; " % base, "a",
                "%s = %s; " % (_r(ins.rn), "a %s %s" % ("+" if ins.add else "-", off) if off else "a"))

    def _load_value(self, op, a):
        if op in ("ldr", "ldrt"):
            return "ldr32(%s)" % a
        if op in ("ldrb", "ldrbt"):
            return "ld8(%s)" % a
        if op == "ldrh":
            return "ldrh(%s)" % a
        if op == "ldrsh":
            return "ldrsh(%s)" % a
        if op == "ldrsb":
            return "(uint32_t)(int8_t)ld8(%s)" % a
        raise Unsupported(op)

    def _ldst(self, ins):
        op = ins.op
        if op in arm.LOADS:
            lit = self._literal(ins) if ins.literal_size else None
            if lit is not None:
                return "%s = %s;" % (_r(ins.rd), _h(lit))
            pre, a, wb = self._addr(ins)
            if op == "ldrd":
                if not pre:
                    pre, a = "const uint32_t a = %s; " % a, "a"
                return "{ %s%s%s = ld32(a & ~3u); %s = ld32((a & ~3u) + 4); }" % (
                    pre, wb, _r(ins.rd), _r(ins.rd + 1))
            v = self._load_value(op, a)
            if not pre:
                return "%s = %s;" % (_r(ins.rd), v)
            return "{ %s%s%s = %s; }" % (pre, wb, _r(ins.rd), v)
        pre, a, wb = self._addr(ins)
        v = _reg(ins, ins.rd, 4)
        if op == "strd":
            if not pre:
                pre, a = "const uint32_t a = %s; " % a, "a"
            return "{ %sconst uint32_t v0 = %s, v1 = %s; st32(a & ~3u, v0); st32((a & ~3u) + 4, v1); %s}" % (
                pre, _r(ins.rd), _reg(ins, ins.rd + 1, 4), wb)
        st = {"str": "st32(%s & ~3u, v)", "strt": "st32(%s & ~3u, v)", "strb": "st8(%s, v)",
              "strbt": "st8(%s, v)", "strh": "st16(%s & ~1u, v)"}[op] % (
            "(%s)" % a if " " in a else a)
        if not pre:
            return "{ const uint32_t v = %s; %s; }" % (v, st)
        return "{ %sconst uint32_t v = %s; %s; %s}" % (pre, v, st, wb)

    def _block(self, ins):
        """(preamble with `a` the first address, address after writeback) of ldm/stm."""
        n = bin(ins.reglist).count("1")
        rn = _r(ins.rn)
        first = {"ia": "", "ib": " + 4", "da": " - %d" % (4 * n - 4), "db": " - %d" % (4 * n)}[ins.mode]
        new = {"ia": " + %d" % (4 * n), "ib": " + %d" % (4 * n), "da": " - %d" % (4 * n),
               "db": " - %d" % (4 * n)}[ins.mode]
        if first == " - 0":
            first = ""
        return "const uint32_t b0 = %s; const uint32_t a = (b0%s) & ~3u; " % (rn, first), "b0" + new

    def _ldm(self, ins):
        """ldm without pc (with pc: a transfer, see _transfer)."""
        rl, rn = ins.reglist, ins.rn
        pre, new = self._block(ins)
        user = ins.user
        out = [pre]
        k = 0
        for i in range(16):
            if rl >> i & 1:
                v = "ld32(a + %d)" % (4 * k) if k else "ld32(a)"
                out.append("arm_set_user_reg(c, %d, %s); " % (i, v) if user else "%s = %s; " % (_r(i), v))
                k += 1
        if ins.writeback and self._ldm_wb(ins):
            out.append("%s = %s; " % (_r(rn), new))
        return "{ %s}" % "".join(out)

    def _ldm_wb(self, ins):
        """Whether ldm writes the base back (rules in armemu)."""
        rl, rn = ins.reglist, ins.rn
        if not rl >> rn & 1:
            return True
        return self.arch >= 5 and not ins.thumb and (rl == 1 << rn or bool(rl >> (rn + 1)))

    def _stm(self, ins):
        rl, rn = ins.reglist, ins.rn
        pre, new = self._block(ins)
        out = [pre]
        lowest = (rl & -rl).bit_length() - 1
        k = 0
        for i in range(16):
            if rl >> i & 1:
                if ins.user:
                    v = "arm_user_reg(c, %d)" % i
                elif i == 15:
                    v = _h(_pcv(ins, 4))
                elif i == rn and ins.writeback and self.arch < 5 and i != lowest:
                    v = "(%s)" % new
                else:
                    v = _r(i)
                out.append("st32(a + %d, %s); " % (4 * k, v) if k else "st32(a, %s); " % v)
                k += 1
        if ins.writeback:
            out.append("%s = %s; " % (_r(rn), new))
        return "{ %s}" % "".join(out)

    def _misc(self, ins, live):
        op, R = ins.op, _r
        if op in ("qadd", "qsub", "qdadd", "qdsub"):
            return "%s = arm_%s(c, %s, %s);" % (R(ins.rd), op, R(ins.rm), R(ins.rn))
        if op == "clz":
            return "%s = %s ? (uint32_t)__builtin_clz(%s) : 32u;" % (R(ins.rd), R(ins.rm), R(ins.rm))
        if op in ("swp", "swpb"):
            if op == "swpb":
                return "{ const uint32_t a = %s, v = ld8(a); st8(a, %s); %s = v; }" % (R(ins.rn), R(ins.rm), R(ins.rd))
            return "{ const uint32_t a = %s, v = ldr32(a); st32(a & ~3u, %s); %s = v; }" % (R(ins.rn), R(ins.rm), R(ins.rd))
        if op == "mrs":
            return "%s = %s;" % (R(ins.rd), "arm_get_spsr(c)" if ins.spsr else "arm_get_cpsr(c)")
        if op == "msr":
            v = _h(ins.imm) if ins.imm is not None else R(ins.rm)
            if ins.spsr:
                return "arm_set_spsr(c, %s, %d);" % (v, ins.mask)
            if ins.mask & 1:
                return "arm_set_cpsr(c, %s, %d); ARM_POLL(c);" % (v, ins.mask)
            return "arm_set_cpsr(c, %s, %d);" % (v, ins.mask)
        if op in ("mcr", "mrc"):
            if ins.cp != 15:
                return "arm_undefined(c, %s);" % _h(ins.pc | self.T)
            args = "%d, %d, %d, %d" % (ins.op1, ins.crn, ins.crm, ins.op2)
            if op == "mcr":
                return "arm_cp15_write(c, %s, %s);" % (args, _reg(ins, ins.rd, 4))
            if ins.rd == 15:
                return "arm_set_nzcv(c, arm_cp15_read(c, %s));" % args
            return "%s = arm_cp15_read(c, %s);" % (R(ins.rd), args)
        if op == "swi":
            num = ins.imm & 0xFF if ins.thumb else (ins.imm >> 16) & 0xFF
            return "arm_swi(c, %d, %s);" % (num, _h((ins.pc + ins.size) | self.T))
        if op in ("pld", "nop"):
            return ";"
        return "arm_undefined(c, %s);" % _h(ins.pc | self.T)

    def stmt(self, ins, live):
        """C++ for one instruction that does not write pc, without its condition."""
        op = ins.op
        if op in arm.DATA_OPS:
            return self._dp(ins, live)
        if op in ("mul", "mla", "umull", "umlal", "smull", "smlal") or op.startswith(("smla", "smul")):
            return self._mul(ins, live)
        if op in arm.LOADS or op in arm.STORES:
            return self._ldst(ins)
        if op == "ldm":
            return self._ldm(ins)
        if op == "stm":
            return self._stm(ins)
        return self._misc(ins, live)

    # ---- transfers --------------------------------------------------------
    @staticmethod
    def _exception_return(ins):
        """A write to pc that also restores the CPSR from the SPSR."""
        return ins.writes_pc and ((ins.op in arm.DATA_OPS and ins.s) or (ins.op == "ldm" and ins.user))

    def _key(self, x, iw):
        """C++ for the address a value written to pc goes to, bit 0 the mode."""
        if iw:
            return "arm_iw(%s)" % x
        return "(%s & ~1u) | 1u" % x if self.thumb else "%s & ~3u" % x

    def _goto(self, t, src):
        if t not in self.f.code:
            return self._tail(t | self.T)
        if t <= src:
            return "ARM_POLL(c); goto L_%08X;" % t
        return "goto L_%08X;" % t

    def _tail(self, key):
        """Leave for `key` (address | thumb), a static target outside this function's code."""
        if key in self.entries:
            return "%s(c); return;" % fname(key & ~1, key & 1)
        self.unknown.append(key)
        return "arm_jump(c, %s); return;" % _h(key)

    def _call(self, key):
        if key in self.entries:
            return "%s(c);" % fname(key & ~1, key & 1)
        self.unknown.append(key)
        return "arm_call(c, %s);" % _h(key)

    def _dcall(self, target, how):
        """The call through `x` (a key), guarded by what analysis expects."""
        self.sites[how] = self.sites.get(how, 0) + 1
        if target is None:
            return "arm_call(c, x);"
        k = target if target & 1 else target & ~3
        if k not in self.entries:
            self.unknown.append(k)
            return "arm_call(c, x);"
        return "if (x == %s) %s(c); else arm_call(c, x);" % (_h(k), fname(k & ~1, k & 1))

    def _back(self):
        """After a jump: control returned to one of this function's own lr
        addresses goes on there; anything else returns."""
        if not self.landings:
            return "return;"
        cases = " ".join("case %s: goto L_%08X;" % (_h(t | self.T), t) for t in sorted(self.landings))
        return "switch (c.pc) { %s } return;" % cases

    def _return(self, key_expr):
        if not self.landings:
            return "c.pc = %s; return;" % key_expr
        cases = " ".join("case %s: goto L_%08X;" % (_h(t | self.T), t) for t in sorted(self.landings))
        return "{ const uint32_t x = %s; switch (x) { %s } c.pc = x; return; }" % (key_expr, cases)

    def _value(self, ins):
        """(preamble, C++ value written to pc, interworks?) of a pc-writing
        instruction other than b/bl/blx-immediate."""
        op = ins.op
        if op in ("bx", "blx"):
            if ins.rm == 15:
                return "", _h(_pcv(ins)), True
            return "", _r(ins.rm), True
        if op in arm.DATA_OPS:
            a = self._base(ins) if op not in ("mov", "mvn") else None
            b, _, pre = self._op2(ins, False)
            if op in ("rsb", "rsc"):
                a, b = b, a
            e = {"and": "%s & %s", "eor": "%s ^ %s", "orr": "%s | %s", "bic": "%s & ~%s",
                 "add": "%s + %s", "sub": "%s - %s", "rsb": "%s - %s",
                 "adc": "%s + %s + c.c", "sbc": "%s - %s - !c.c", "rsc": "%s - %s - !c.c"}
            if op == "mov":
                v = b
            elif op == "mvn":
                v = "~(%s)" % b
            else:
                v = e[op] % (a, "(%s)" % b)
            return pre, "(uint32_t)(%s)" % v, False
        if op == "ldr":
            lit = self._literal(ins) if ins.literal_size else None
            if lit is not None:
                return "", _h(lit), self.arch >= 5
            pre, a, wb = self._addr(ins)
            if pre:
                return "%s%sconst uint32_t v = ldr32(a); " % (pre, wb), "v", self.arch >= 5
            return "const uint32_t v = ldr32(%s); " % a, "v", self.arch >= 5
        if op == "ldm":
            rl, rn = ins.reglist, ins.rn
            pre, new = self._block(ins)
            out = [pre]
            k = 0
            for i in range(15):
                if rl >> i & 1:
                    v = "ld32(a + %d)" % (4 * k) if k else "ld32(a)"
                    out.append("%s = %s; " % (_r(i), v))
                    k += 1
            out.append("const uint32_t v = ld32(a + %d); " % (4 * k) if k else "const uint32_t v = ld32(a); ")
            if ins.writeback and self._ldm_wb(ins):
                out.append("%s = %s; " % (_r(rn), new))
            return "".join(out), "v", self.arch >= 5
        raise Unsupported("%08X: %s" % (ins.pc, ins.text))

    def _transfer(self, a, ins):
        """(C++ without the condition, falls through?) of an instruction that writes pc."""
        op = ins.op
        nxt = a + ins.size
        T = self.T
        if op == "b":
            return self._goto(ins.target, a), False
        if op in ("bl", "blx") and ins.target is not None:
            to_thumb = (not self.thumb) if op == "blx" else self.thumb
            ret = nxt | T
            return ("%s = %s; ARM_POLL(c); %s ARM_RET(c, %s);"
                    % (_r(14), _h(ret), self._call(ins.target | int(to_thumb)), _h(ret))), True
        if op in ("bl_hi", "bl_lo", "blx_lo"):
            raise Unsupported("%08X: a lone half of a Thumb bl" % a)
        pre, v, iw = self._value(ins)
        if self._exception_return(ins):          # subs pc, lr, #4 / ldm ..., {..., pc}^
            return "{ %sarm_exception_return(c, %s); return; }" % (pre, v), False
        key = self._key(v, iw)
        kind, target, ret = self.f.sites.get(a, ("call" if op == "blx" else "jump", None, None))
        if op == "blx":                          # blx rm: lr is set here
            ret = nxt | T
            return ("{ %sconst uint32_t x = %s; %s = %s; ARM_POLL(c); %s ARM_RET(c, %s); }"
                    % (pre, key, _r(14), _h(ret), self._dcall(target, "resolved" if target is not None
                                                              else "unresolved"), _h(ret))), True
        if kind == "return":
            return "{ %s%s }" % (pre, self._return(key)), False
        if kind == "switch":
            targets = [t for t in self.p.switches.get(a, ()) if t in self.f.code
                       and (ret is None or t != ret & ~1)]
            back = any(t <= a for t in targets)
            cases = " ".join("case %s: goto L_%08X;" % (_h(t | T), t) for t in dict.fromkeys(targets))
            self.sites["switch"] = self.sites.get("switch", 0) + 1
            return ("{ %sconst uint32_t x = %s; %sswitch (x) { %s default: arm_jump(c, x); %s } }"
                    % (pre, key, "ARM_POLL(c); " if back else "", cases, self._back())), False
        if kind == "call":
            how = "resolved" if target is not None else "unresolved"
            r = ret if ret is not None else nxt | T
            if (r & ~1) == nxt:
                after = ""
            elif (r & ~1) in self.f.code:
                after = " goto L_%08X;" % (r & ~1)
            else:                                 # lr is another function's entry: go on there
                after = " " + self._tail(r if r & 1 else r & ~3)
            return ("{ %sconst uint32_t x = %s; ARM_POLL(c); %s ARM_RET(c, %s);%s }"
                    % (pre, key, self._dcall(target, how), _h(r), after)), (r & ~1) == nxt
        # a jump
        if target is not None:
            k = target if target & 1 else target & ~3
            self.sites["tail"] = self.sites.get("tail", 0) + 1
            if (k & ~1) in self.f.code and (k & 1) == T:
                return "{ %s%s }" % (pre, self._goto(k & ~1, a)), False
            if k in self.entries:
                return ("{ %sconst uint32_t x = %s; if (x == %s) %s(c); else arm_jump(c, x); %s }"
                        % (pre, key, _h(k), fname(k & ~1, k & 1), self._back())), False
        self.sites["unresolved jump"] = self.sites.get("unresolved jump", 0) + 1
        return "{ %sarm_jump(c, %s); %s }" % (pre, key, self._back()), False

    # ---- the function -----------------------------------------------------
    def labels(self):
        out = set(self.landings)
        code = self.f.code
        for a, ins in self.ops.items():
            if ins.op == "b" and ins.target in code:
                out.add(ins.target)
            site = self.f.sites.get(a)
            if site is None:
                continue
            kind, target, ret = site
            if kind == "switch":
                out |= {t for t in self.p.switches.get(a, ()) if t in code}
            if ret is not None and (ret & ~1) in code and (ret & ~1) != a + ins.size:
                out.add(ret & ~1)
            if kind == "jump" and target is not None and (target & ~1) in code:
                out.add(target & ~1 if target & 1 else target & ~3)
        return out

    def _insn(self, a):
        """(C++ for the instruction at `a`, the address it continues at or None)."""
        ins = self.ops[a]
        nxt = a + ins.size
        if ins.writes_pc or ins.op == "blx" or ins.is_call:
            text, falls = self._transfer(a, ins)
            if ins.cond != 14:
                return "if (%s) { %s }" % (COND_EXPR[ins.cond], text), nxt
            return text, (nxt if falls else None)
        text = self.stmt(ins, self.live[a])
        if ins.cond != 14 and text != ";":
            return "if (%s) %s" % (COND_EXPR[ins.cond], text if text.startswith("{") else "{ %s }" % text), nxt
        return text, nxt

    def emit(self):
        """The C++ function, as a list of lines."""
        labels = self.labels()
        code = self.f.code
        order = sorted(code)
        lines = ["void %s(ArmContext& c) {" % fname(self.f.entry, self.thumb)]
        if order[0] != self.f.entry:              # code shared from below the entry
            labels.add(self.f.entry)
            lines.append("    goto L_%08X;" % self.f.entry)
        late = set()
        for i, a in enumerate(order):
            if a in labels or a in late:
                lines.append("L_%08X:" % a)
            text, succ = self._insn(a)
            if self.comments:
                text += "  // %08X %s" % (a, self.ops[a].text.split("  ;")[0])
            lines.append("    " + text)
            nxt = order[i + 1] if i + 1 < len(order) else None
            if succ is not None and succ != nxt:
                if succ in code:
                    late.add(succ)
                    lines.append("    goto L_%08X;" % succ)
                else:
                    lines.append("    " + self._tail(succ | self.T))
        lines.append("}")        # (a fall-through goes forward: a late label is printed when reached)
        return lines


def _shift_imm(kind, n, m):
    """C++ for register m shifted by an immediate (the decoder's encoding)."""
    if kind is None:
        return m
    if kind == "lsl":
        return "(%s << %d)" % (m, n)
    if kind == "lsr":
        return "(%s >> %d)" % (m, n) if n < 32 else "0u"
    if kind == "asr":
        return "(uint32_t)((int32_t)%s >> %d)" % (m, min(n, 31))
    if kind == "ror":
        return "arm_ror(%s, %d)" % (m, n)
    return "(c.c << 31 | %s >> 1)" % m          # rrx


def _shift_imm_c(kind, n, m):
    """C++ for the carry out of an immediate shift, None when C stays."""
    if kind is None:
        return None
    if kind == "lsl":
        return "(%s >> %d & 1u)" % (m, 32 - n)
    if kind in ("lsr", "asr"):
        return "(%s >> %d & 1u)" % (m, n - 1) if n < 32 else "(%s >> 31)" % m
    if kind == "ror":
        return "(%s >> %d & 1u)" % (m, n - 1)
    return "(%s & 1u)" % m                      # rrx

