// ndskit runtime — the interpreter: ARM and Thumb code run from memory as it
// is when called, for code no module holds (decrypted, or copied at run time
// without --copy) and functions the emitter could not take.
//
// Semantics are ndskit/armemu.py's, instruction for instruction, as the
// recompiled code's are; decoding is ndskit/arm.py's. The self-test's
// --interp runs every vector through it (selftest.cpp).
//
// arm_interpret(c, key) runs from key until control comes back to lr as it was
// at the call. On the way:
//   * a call (bl, blx, or a transfer that set lr) to an entry some module
//     vouches for (arm_lookup) runs the recompiled function; its return goes on
//     where it went, interpreted;
//   * a jump to an entry is a tail call: the function runs, then its return
//     goes on as above (to lr: done);
//   * a jump through a register into a program's own code that is no entry,
//     from code outside it, is a return elsewhere: c.pc says where, and the
//     caller's check (ARM_RET) decides;
//   * safe points at calls and backward jumps, as in recompiled code.
#include "arm.h"

int g_arm_interp_only;                  // the self-test's --interp: no recompiled code, callees interpreted too

namespace {

inline uint32_t sext(uint32_t v, int bits) { return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits)); }

// The barrel shifter: (result, carry out into cy). `n` as encoded (lsr/asr #32
// as 32, rrx as kind 4), or Rs's bottom byte when by_reg.
inline uint32_t shift_c(int kind, uint32_t v, uint32_t n, uint32_t& cy, bool by_reg) {
    if (kind < 0) return v;
    if (kind == 4) { uint32_t o = v & 1; v = cy << 31 | v >> 1; cy = o; return v; }
    if (by_reg && n == 0) return v;
    switch (kind) {
    case 0:
        if (n < 32) { cy = v >> (32 - n) & 1; return v << n; }
        cy = n == 32 ? v & 1 : 0;
        return 0;
    case 1:
        if (n < 32) { cy = v >> (n - 1) & 1; return v >> n; }
        cy = n == 32 ? v >> 31 : 0;
        return 0;
    case 2:
        if (n < 32) { cy = v >> (n - 1) & 1; return (uint32_t)((int32_t)v >> n); }
        cy = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        if ((n & 31) == 0) { cy = v >> 31; return v; }
        v = arm_ror(v, n);
        cy = v >> 31;
        return v;
    }
}

// An immediate shift's (kind, amount) from bits 11-5: kind -1 none, 4 rrx.
inline void imm_shift(uint32_t w, int& kind, uint32_t& n) {
    n = w >> 7 & 31;
    int t = w >> 5 & 3;
    if (t == 0) { kind = n ? 0 : -1; return; }
    if (t == 3 && n == 0) { kind = 4; return; }
    kind = t;
    if (!n) n = 32;
}

inline bool cond_ok(const ArmContext& c, uint32_t cc) {
    switch (cc) {
    case 0: return c.z;
    case 1: return !c.z;
    case 2: return c.c;
    case 3: return !c.c;
    case 4: return c.n;
    case 5: return !c.n;
    case 6: return c.v;
    case 7: return !c.v;
    case 8: return c.c && !c.z;
    case 9: return !c.c || c.z;
    case 10: return c.n == c.v;
    case 11: return c.n != c.v;
    case 12: return !c.z && c.n == c.v;
    case 13: return c.z || c.n != c.v;
    default: return true;
    }
}

inline int32_t half(uint32_t v, bool top) { return (int32_t)(int16_t)(top ? v >> 16 : v); }

template <int CPU> struct Interp {
    static constexpr int kArch = CPU == 9 ? 5 : 4;
    ArmContext& c;
    uint32_t exit;                      // lr at the call: back there, it is done
    bool from_program;                  // the code began inside a program's own segment
    uint32_t pc;                        // the instruction being executed
    bool t;                             // Thumb
    // what the instruction did to the flow
    enum { NEXT, JUMP, CALL, ERET } flow;
    uint32_t to;                        // JUMP/CALL: the address, bit 0 the state
    bool reg;                           // JUMP: through a register (not an immediate branch)

    static uint32_t ld8(uint32_t a) { return nds_ld8<CPU>(a); }
    static uint32_t ld16(uint32_t a) { return nds_ld16<CPU>(a); }
    static uint32_t ld32(uint32_t a) { return nds_ld32<CPU>(a); }
    static void st8(uint32_t a, uint32_t v) { nds_st8<CPU>(a, v); }
    static void st16(uint32_t a, uint32_t v) { nds_st16<CPU>(a, v); }
    static void st32(uint32_t a, uint32_t v) { nds_st32<CPU>(a, v); }
    static uint32_t ldr32(uint32_t a) { return arm_ror(ld32(a & ~3u), (a & 3u) * 8); }
    static uint32_t ldrh(uint32_t a) {
        if (CPU == 9 || !(a & 1)) return ld16(a & ~1u);
        return arm_ror(ld16(a & ~1u), 8);
    }
    static uint32_t ldrsh(uint32_t a) {
        if (CPU == 9 || !(a & 1)) return (uint32_t)(int16_t)ld16(a & ~1u);
        return (uint32_t)(int8_t)ld8(a);
    }

    uint32_t key() const { return t ? pc | 1u : pc; }

    // Register n as an operand: pc reads +8 (ARM; +12 for a register-specified
    // shift and a stored pc) or +4 (Thumb).
    uint32_t rv(int n, uint32_t extra = 0) const {
        if (n == 15) return pc + (t ? 4 : 8 + extra);
        return c.r[n];
    }

    // pc = target; with interwork bit 0 picks the state (armemu's branch).
    void branch(uint32_t target, bool interwork) {
        bool nt = interwork ? (target & 1) : t;
        to = nt ? (target & ~1u) | 1u : target & ~3u;
        flow = JUMP;
        reg = true;
    }
    void write_pc(uint32_t v) { branch(v, kArch >= 5); }
    void link_to(uint32_t target_key) { to = target_key; flow = CALL; }

    void undefined() { arm_undefined(c, key()); }

    // ---- data processing ----------------------------------------------------------
    // op: and eor sub rsb add adc sbc rsc tst teq cmp cmn orr mov bic mvn
    void dp(int op, bool s, int rd, uint32_t a, uint32_t b, uint32_t carry) {
        uint32_t r;
        bool logic = false, write = true;
        uint64_t full = 0;
        uint32_t x = 0, y = 0;
        switch (op) {
        case 0: r = a & b; logic = true; break;
        case 1: r = a ^ b; logic = true; break;
        case 8: r = a & b; logic = true; write = false; break;
        case 9: r = a ^ b; logic = true; write = false; break;
        case 12: r = a | b; logic = true; break;
        case 13: r = b; logic = true; break;
        case 14: r = a & ~b; logic = true; break;
        case 15: r = ~b; logic = true; break;
        default: {
            uint32_t ci;
            switch (op) {
            case 2: case 10: x = a; y = ~b; ci = 1; break;            // sub, cmp
            case 3: x = b; y = ~a; ci = 1; break;                      // rsb
            case 4: case 11: x = a; y = b; ci = 0; break;              // add, cmn
            case 5: x = a; y = b; ci = c.c; break;                     // adc
            case 6: x = a; y = ~b; ci = c.c; break;                    // sbc
            default: x = b; y = ~a; ci = c.c; break;                   // rsc
            }
            full = (uint64_t)x + y + ci;
            r = (uint32_t)full;
            write = op != 10 && op != 11;
        }
        }
        if (logic) {
            if (!write || (s && rd != 15)) { c.n = r >> 31; c.z = r == 0; c.c = carry; }
        } else if (!write || (s && rd != 15)) {
            c.n = r >> 31; c.z = r == 0; c.c = (uint32_t)(full >> 32); c.v = ((x ^ r) & (y ^ r)) >> 31;
        }
        if (!write) return;
        if (rd == 15) {
            if (s) { arm_exception_return(c, r); flow = ERET; return; }
            branch(r, false);
            return;
        }
        c.r[rd] = r;
    }

    // ---- loads and stores -----------------------------------------------------------
    // op: 0 ldr, 1 ldrb, 2 ldrh, 3 ldrsh, 4 ldrsb, 5 ldrd; 8 str, 9 strb, 10 strh, 11 strd
    void ldst(int op, int rd, int rn, uint32_t base, uint32_t off, bool pre, bool add, bool wb) {
        uint32_t moved = add ? base + off : base - off;
        uint32_t a = pre ? moved : base;
        bool w = wb && rn != 15;
        if (op < 8) {
            uint32_t v;
            switch (op) {
            case 0: v = ldr32(a); break;
            case 1: v = ld8(a); break;
            case 2: v = ldrh(a); break;
            case 3: v = ldrsh(a); break;
            case 4: v = (uint32_t)(int8_t)ld8(a); break;
            default: {
                uint32_t lo = ld32(a & ~3u), hi = ld32((a & ~3u) + 4);
                if (w) c.r[rn] = moved;
                c.r[rd] = lo;
                c.r[rd + 1] = hi;
                return;
            }
            }
            if (w) c.r[rn] = moved;                 // the loaded value wins over the base
            if (rd == 15) write_pc(v);
            else c.r[rd] = v;
            return;
        }
        uint32_t v = rv(rd, 4);                     // str pc: + 12
        switch (op) {
        case 8: st32(a & ~3u, v); break;
        case 9: st8(a, v); break;
        case 10: st16(a & ~1u, v); break;
        default: st32(a & ~3u, c.r[rd]); st32((a & ~3u) + 4, rv(rd + 1, 4)); break;
        }
        if (w) c.r[rn] = moved;
    }

    // mode: 0 da, 1 ia, 2 db, 3 ib
    void block(bool load, int rn, uint32_t rl, int mode, bool wb, bool user, bool thumb) {
        uint32_t n = (uint32_t)__builtin_popcount(rl);
        uint32_t base = c.r[rn], a, nb;
        switch (mode) {
        case 1: a = base; nb = base + 4 * n; break;
        case 3: a = base + 4; nb = base + 4 * n; break;
        case 0: a = base - 4 * n + 4; nb = base - 4 * n; break;
        default: a = base - 4 * n; nb = base - 4 * n; break;
        }
        a &= ~3u;
        if (load) {
            uint32_t vals[16];
            for (int i = 0, k = 0; i < 16; ++i)
                if (rl >> i & 1) vals[i] = ld32(a + 4 * k++);
            bool u = user && !(rl & 0x8000);
            for (int i = 0; i < 15; ++i)
                if (rl >> i & 1) {
                    if (u) arm_set_user_reg(c, i, vals[i]);
                    else c.r[i] = vals[i];
                }
            if (wb) {
                if (!(rl >> rn & 1)) c.r[rn] = nb;
                else if (kArch >= 5 && !thumb && (rl == 1u << rn || (rl >> (rn + 1)))) c.r[rn] = nb;
            }
            if (rl & 0x8000) {
                if (user) { arm_exception_return(c, vals[15]); flow = ERET; }
                else write_pc(vals[15]);
            }
            return;
        }
        int lowest = __builtin_ctz(rl);
        for (int i = 0, k = 0; i < 16; ++i) {
            if (!(rl >> i & 1)) continue;
            uint32_t v;
            if (user) v = arm_user_reg(c, i);
            else if (i == 15) v = rv(15, 4);
            else if (i == rn && wb && kArch < 5 && i != lowest) v = nb;
            else v = c.r[i];
            st32(a + 4 * k++, v);
        }
        if (wb) c.r[rn] = nb;
    }

    // ---- ARM ---------------------------------------------------------------------------
    void arm(uint32_t w) {
        uint32_t cond = w >> 28;
        if (cond == 15) { arm_nv(w); return; }
        if (cond != 14 && !cond_ok(c, cond)) return;
        uint32_t k = w >> 25 & 7;
        int rn = w >> 16 & 15, rd = w >> 12 & 15, rs = w >> 8 & 15, rm = w & 15;
        switch (k) {
        case 0:
            if ((w & 0x90) == 0x90) { mul_extra(w); return; }
            if ((w & 0x01900000) == 0x01000000) { misc(w); return; }
            [[fallthrough]];
        case 1: {
            if (k == 1 && (w & 0x01900000) == 0x01000000) {
                if (!(w & 0x00200000) || (w & 0xF000) != 0xF000) { undefined(); return; }
                uint32_t mask = w >> 16 & 15;
                if (!mask && !(w & 0x00400000)) return;               // nop
                uint32_t v = arm_ror(w & 0xFF, (w >> 8 & 15) * 2);
                msr(w & 0x00400000, mask, v);
                return;
            }
            int op = w >> 21 & 15;
            bool s = w & 0x00100000;
            uint32_t b, carry = c.c, a;
            if (k == 1) {
                uint32_t rot = (w >> 8 & 15) * 2;
                b = arm_ror(w & 0xFF, rot);
                if (rot) carry = b >> 31;
                a = rv(rn);
            } else if (w & 0x10) {                                     // shift by a register
                b = shift_c(w >> 5 & 3, rv(rm, 4), c.r[rs] & 0xFF, carry, true);
                a = rv(rn, 4);
            } else {
                int kind; uint32_t n;
                imm_shift(w, kind, n);
                b = shift_c(kind, rv(rm), n, carry, false);
                a = rv(rn);
            }
            dp(op, s || (op >= 8 && op <= 11), rd, a, b, carry);
            return;
        }
        case 2: case 3: {
            if (k == 3 && (w & 0x10)) { undefined(); return; }
            bool p = w >> 24 & 1, u = w >> 23 & 1, b = w >> 22 & 1, wb = w >> 21 & 1, ld = w >> 20 & 1;
            uint32_t off;
            if (k == 3) {
                int kind; uint32_t n;
                imm_shift(w, kind, n);
                uint32_t cy = c.c;
                off = shift_c(kind, rv(rm), n, cy, false);
            } else off = w & 0xFFF;
            ldst(ld ? (b ? 1 : 0) : (b ? 9 : 8), rd, rn, rv(rn), off, p, u, wb || !p);
            return;
        }
        case 4: {
            uint32_t rl = w & 0xFFFF;
            if (!rl) { undefined(); return; }
            int p = w >> 24 & 1, u = w >> 23 & 1;
            block(w >> 20 & 1, rn, rl, p * 2 + u, w >> 21 & 1, w >> 22 & 1, false);
            return;
        }
        case 5: {
            uint32_t target = pc + 8 + (sext(w & 0xFFFFFF, 24) << 2);
            if (w & 0x01000000) { c.r[14] = pc + 4; link_to(target & ~3u); }
            else { to = target & ~3u; flow = JUMP; reg = false; }
            return;
        }
        case 6: undefined(); return;                                   // ldc/stc/mcrr
        default:
            if (w & 0x01000000) { arm_swi(c, (w >> 16) & 0xFF, pc + 4); return; }
            if (!(w & 0x10)) { undefined(); return; }                  // cdp
            cop(w);
            return;
        }
    }

    void arm_nv(uint32_t w) {
        if (kArch < 5) { undefined(); return; }
        if ((w & 0x0E000000) == 0x0A000000) {                           // blx imm
            uint32_t off = sext(w & 0xFFFFFF, 24) << 2 | (w >> 23 & 2);
            c.r[14] = pc + 4;
            link_to((pc + 8 + off) | 1u);
            return;
        }
        if ((w & 0x0D70F000) == 0x0550F000) {                           // pld
            if ((w & 0x02000000) && (w & 0x10)) undefined();
            return;
        }
        undefined();                                                    // mcr2/mrc2, cdp2, ldc2...
    }

    void cop(uint32_t w) {
        if ((w >> 8 & 15) != 15) { undefined(); return; }
        int op1 = w >> 21 & 7, rd = w >> 12 & 15, crn = w >> 16 & 15, crm = w & 15, op2 = w >> 5 & 7;
        if (!(w & 0x00100000)) { arm_cp15_write(c, op1, crn, crm, op2, rv(rd, 4)); return; }
        uint32_t v = arm_cp15_read(c, op1, crn, crm, op2);
        if (rd == 15) arm_set_nzcv(c, v);
        else c.r[rd] = v;
    }

    void msr(bool spsr, uint32_t mask, uint32_t v) {
        if (spsr) { arm_set_spsr(c, v, mask); return; }
        arm_set_cpsr(c, v, mask);
        if (mask & 1) ARM_POLL(c);
    }

    void mul_extra(uint32_t w) {
        uint32_t sh = w >> 5 & 3;
        int r19 = w >> 16 & 15, r15 = w >> 12 & 15, rs = w >> 8 & 15, rm = w & 15;
        if (sh == 0) {
            if (!(w & 0x0F000000)) {
                uint32_t op = w >> 21 & 7;
                bool s = w & 0x00100000;
                if (op == 0 || op == 1) {
                    uint32_t r = c.r[rm] * c.r[rs];
                    if (op == 1) r += c.r[r15];
                    c.r[r19] = r;
                    if (s) { c.n = r >> 31; c.z = r == 0; }
                    return;
                }
                if (op < 4) { undefined(); return; }
                uint64_t p;
                if (op & 2) p = (uint64_t)((int64_t)(int32_t)c.r[rm] * (int32_t)c.r[rs]);
                else p = (uint64_t)c.r[rm] * c.r[rs];
                if (op & 1) p += (uint64_t)c.r[r19] << 32 | c.r[r15];
                c.r[r15] = (uint32_t)p;
                c.r[r19] = (uint32_t)(p >> 32);
                if (s) { c.n = (uint32_t)(p >> 63); c.z = p == 0; }
                return;
            }
            if ((w & 0x0FB00000) == 0x01000000) {                       // swp, swpb
                uint32_t a = c.r[r19], v;
                if (w & 0x00400000) { v = ld8(a); st8(a, c.r[rm]); }
                else { v = ldr32(a); st32(a & ~3u, c.r[rm]); }
                c.r[r15] = v;
                return;
            }
            undefined();
            return;
        }
        bool p = w >> 24 & 1, u = w >> 23 & 1, i = w >> 22 & 1, wb = w >> 21 & 1, ld = w >> 20 & 1;
        if (!p && wb) { undefined(); return; }
        int op;
        if (ld) op = sh == 1 ? 2 : sh == 2 ? 4 : 3;
        else {
            if (sh >= 2 && (kArch < 5 || r15 == 15)) { undefined(); return; }
            op = sh == 1 ? 10 : sh == 2 ? 5 : 11;
        }
        uint32_t off = i ? ((w >> 4 & 0xF0) | (w & 15)) : c.r[rm];
        if (!i && rm == 15) off = rv(15);
        ldst(op, r15, r19, rv(r19), off, p, u, wb || !p);
    }

    void misc(uint32_t w) {
        uint32_t op = w >> 21 & 3, b74 = w >> 4 & 15;
        int rd = w >> 12 & 15, rn = w >> 16 & 15, rs = w >> 8 & 15, rm = w & 15;
        if (b74 == 0) {
            if (!(op & 1)) {
                if ((w & 0x000F0FFF) != 0x000F0000) { undefined(); return; }
                c.r[rd] = (op & 2) ? arm_get_spsr(c) : arm_get_cpsr(c);
                return;
            }
            if ((w & 0xFFF0) != 0xF000) { undefined(); return; }
            msr(op & 2, (uint32_t)rn, c.r[rm]);
            return;
        }
        bool sbo = (w & 0x000FFF00) == 0x000FFF00;
        if (b74 == 1 && op == 1 && sbo) { branch(rv(rm), true); return; }    // bx
        if (kArch < 5) { undefined(); return; }
        if (b74 == 1 && op == 3 && (w & 0x000F0F00) == 0x000F0F00) {
            c.r[rd] = c.r[rm] ? (uint32_t)__builtin_clz(c.r[rm]) : 32u;
            return;
        }
        if (b74 == 3 && op == 1 && sbo) {                                // blx rm
            uint32_t x = rv(rm);
            c.r[14] = pc + 4;
            link_to(arm_iw(x));
            return;
        }
        if (b74 == 5) {
            uint32_t a = c.r[rm], b = c.r[rn];
            switch (op) {
            case 0: c.r[rd] = arm_qadd(c, a, b); break;
            case 1: c.r[rd] = arm_qsub(c, a, b); break;
            case 2: c.r[rd] = arm_qdadd(c, a, b); break;
            default: c.r[rd] = arm_qdsub(c, a, b); break;
            }
            return;
        }
        if ((b74 & 9) == 8) {                                            // the DSP multiplies
            bool x = w >> 5 & 1, y = w >> 6 & 1;
            switch (op) {
            case 0: {                                                    // smla<x><y>
                int64_t s = (int64_t)(half(c.r[rm], x) * half(c.r[rs], y)) + (int32_t)c.r[rd];
                if (s != (int32_t)s) c.q = 1;
                c.r[rn] = (uint32_t)s;
                return;
            }
            case 1: {
                int32_t p = (int32_t)(((int64_t)(int32_t)c.r[rm] * half(c.r[rs], y)) >> 16);
                if (x) { c.r[rn] = (uint32_t)p; return; }                // smulw<y>
                int64_t s = (int64_t)p + (int32_t)c.r[rd];               // smlaw<y>
                if (s != (int32_t)s) c.q = 1;
                c.r[rn] = (uint32_t)s;
                return;
            }
            case 2: {                                                    // smlal<x><y>
                int64_t p = (int64_t)half(c.r[rm], x) * half(c.r[rs], y);
                uint64_t s = ((uint64_t)c.r[rn] << 32 | c.r[rd]) + (uint64_t)p;
                c.r[rd] = (uint32_t)s;
                c.r[rn] = (uint32_t)(s >> 32);
                return;
            }
            default:                                                     // smul<x><y>
                c.r[rn] = (uint32_t)(half(c.r[rm], x) * half(c.r[rs], y));
                return;
            }
        }
        undefined();                                                     // bkpt and the rest
    }

    // ---- Thumb -------------------------------------------------------------------------
    // Returns the instruction's size.
    uint32_t thumb(uint32_t h) {
        uint32_t top = h >> 11;
        int lo3 = h & 7, mid3 = h >> 3 & 7;
        if (top < 3) {                                                   // lsl/lsr/asr rd, rm, #imm5
            uint32_t n = h >> 6 & 31, cy = c.c, r;
            if (top == 0) r = n ? shift_c(0, c.r[mid3], n, cy, false) : c.r[mid3];
            else r = shift_c((int)top, c.r[mid3], n ? n : 32, cy, false);
            c.r[lo3] = r; c.n = r >> 31; c.z = r == 0; c.c = cy;
            return 2;
        }
        if (top == 3) {                                                  // add/sub rd, rn, rm|#imm3
            uint32_t b = (h & 0x400) ? (h >> 6 & 7) : c.r[h >> 6 & 7];
            dp((h & 0x200) ? 2 : 4, true, lo3, c.r[mid3], b, c.c);
            return 2;
        }
        if (top < 8) {                                                   // mov/cmp/add/sub rd, #imm8
            static const int ops[4] = {13, 10, 4, 2};
            int rd = h >> 8 & 7;
            dp(ops[top - 4], true, rd, c.r[rd], h & 0xFF, c.c);
            return 2;
        }
        if (top == 8) {
            if (!(h & 0x400)) {                                          // ALU rd, rs
                uint32_t alu = h >> 6 & 15, cy = c.c;
                switch (alu) {
                case 0: dp(0, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                case 1: dp(1, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                case 2: case 3: case 4: case 7: {
                    int kind = alu == 2 ? 0 : alu == 3 ? 1 : alu == 4 ? 2 : 3;
                    uint32_t r = shift_c(kind, c.r[lo3], c.r[mid3] & 0xFF, cy, true);
                    dp(13, true, lo3, 0, r, cy);
                    break;
                }
                case 5: dp(5, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                case 6: dp(6, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                case 8: dp(8, true, 0, c.r[lo3], c.r[mid3], cy); break;
                case 9: dp(3, true, lo3, c.r[mid3], 0, cy); break;       // neg: rsb rd, rn, #0
                case 10: dp(10, true, 0, c.r[lo3], c.r[mid3], cy); break;
                case 11: dp(11, true, 0, c.r[lo3], c.r[mid3], cy); break;
                case 12: dp(12, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                case 13: {
                    uint32_t r = c.r[mid3] * c.r[lo3];
                    c.r[lo3] = r; c.n = r >> 31; c.z = r == 0;
                    break;
                }
                case 14: dp(14, true, lo3, c.r[lo3], c.r[mid3], cy); break;
                default: dp(15, true, lo3, 0, c.r[mid3], cy); break;
                }
                return 2;
            }
            uint32_t op = h >> 8 & 3;
            int rd = lo3 | (h >> 4 & 8), rm = h >> 3 & 15;
            switch (op) {
            case 0: dp(4, false, rd, rv(rd), rv(rm), c.c); break;
            case 1: dp(10, true, 0, rv(rd), rv(rm), c.c); break;
            case 2: dp(13, false, rd, 0, rv(rm), c.c); break;
            default:
                if (h & 0x80) {
                    if (kArch < 5) { undefined(); break; }
                    uint32_t x = rv(rm);
                    c.r[14] = (pc + 2) | 1u;
                    link_to(arm_iw(x));
                } else branch(rv(rm), true);
            }
            return 2;
        }
        if (top == 9) {                                                  // ldr rd, [pc, #imm8*4]
            ldst(0, h >> 8 & 7, 15, (pc + 4) & ~3u, (h & 0xFF) * 4, true, true, false);
            return 2;
        }
        if (top < 12) {                                                  // register offset
            static const int ops[8] = {8, 10, 9, 4, 0, 2, 1, 3};
            ldst(ops[h >> 9 & 7], lo3, mid3, c.r[mid3], c.r[h >> 6 & 7], true, true, false);
            return 2;
        }
        if (top < 18) {                                                  // immediate offset
            static const int ops[6] = {8, 0, 9, 1, 10, 2}, scale[6] = {4, 4, 1, 1, 2, 2};
            ldst(ops[top - 12], lo3, mid3, c.r[mid3], (h >> 6 & 31) * scale[top - 12], true, true, false);
            return 2;
        }
        if (top < 20) {                                                  // str/ldr rd, [sp, #imm8*4]
            ldst(top & 1 ? 0 : 8, h >> 8 & 7, 13, c.r[13], (h & 0xFF) * 4, true, true, false);
            return 2;
        }
        if (top < 22) {                                                  // add rd, pc|sp, #imm8*4
            uint32_t imm = (h & 0xFF) * 4;
            c.r[h >> 8 & 7] = (top & 1 ? c.r[13] : (pc + 4) & ~3u) + imm;
            return 2;
        }
        if (top < 24) {
            if ((h & 0xFF00) == 0xB000) {                                // add/sub sp, #imm7*4
                uint32_t imm = (h & 0x7F) * 4;
                c.r[13] = (h & 0x80) ? c.r[13] - imm : c.r[13] + imm;
                return 2;
            }
            if ((h & 0xF600) == 0xB400) {                                // push / pop
                bool pop = h & 0x800;
                uint32_t rl = (h & 0xFF) | ((h & 0x100) << (pop ? 7 : 6));
                if (!rl) { undefined(); return 2; }
                block(pop, 13, rl, pop ? 1 : 2, true, false, true);
                return 2;
            }
            undefined();
            return 2;
        }
        if (top < 26) {                                                  // stmia/ldmia rn!, {list}
            int rn = h >> 8 & 7;
            uint32_t rl = h & 0xFF;
            if (!rl) { undefined(); return 2; }
            bool ld = top & 1;
            block(ld, rn, rl, 1, !(ld && (rl >> rn & 1)), false, true);
            return 2;
        }
        if (top < 28) {
            uint32_t cond = h >> 8 & 15;
            if (cond == 15) { arm_swi(c, h & 0xFF, (pc + 2) | 1u); return 2; }
            if (cond == 14) { undefined(); return 2; }
            if (cond_ok(c, cond)) { to = (pc + 4 + (sext(h & 0xFF, 8) << 1)) | 1u; flow = JUMP; reg = false; }
            return 2;
        }
        if (top == 28) {
            to = (pc + 4 + (sext(h & 0x7FF, 11) << 1)) | 1u;
            flow = JUMP;
            reg = false;
            return 2;
        }
        if (top == 30) {                                                 // BL/BLX prefix
            uint32_t hi = sext(h & 0x7FF, 11) << 12;
            uint32_t n = ld16(pc + 2);
            if ((n & 0xF800) == 0xF800 || (kArch >= 5 && (n & 0xF801) == 0xE800)) {
                uint32_t target = pc + 4 + hi + ((n & 0x7FF) << 1);
                c.r[14] = (pc + 4) | 1u;
                link_to((n & 0x1000) ? target | 1u : target & ~3u);
                return 4;
            }
            c.r[14] = pc + 4 + hi;                                       // bl_hi
            return 2;
        }
        if (top == 31 || (kArch >= 5 && !(h & 1))) {                     // a lone suffix: pc = lr + imm
            uint32_t target = c.r[14] + ((h & 0x7FF) << 1);
            c.r[14] = (pc + 2) | 1u;
            link_to(top == 31 ? (target & ~1u) | 1u : target & ~3u);
            return 2;
        }
        undefined();
        return 2;
    }

    // ---- the run -----------------------------------------------------------------------
    bool in_program(uint32_t k) const {
        for (int m = 0; m < g_arm_nmodules; ++m) {
            const ArmModule* mod = g_arm_modules[m];
            if (mod->cpu != CPU || mod->overlay >= 0) continue;
            for (uint32_t s = 0; s < mod->nsegs; ++s)
                if (mod->segs[s].kind == ARM_SEG_PROGRAM && (k & ~1u) - mod->segs[s].base < mod->segs[s].size)
                    return true;
        }
        return false;
    }

    // Control goes to `k` (bit 0 the state); `link`: lr was set for a return here.
    // False when the interpreted call is over (c.pc says where it went).
    bool transfer(uint32_t k, bool link, bool by_reg) {
        for (;;) {
            if (!link && k == exit) { c.pc = k; return false; }
            if (!g_arm_interp_only) {
                if (ArmFunc f = arm_lookup(CPU, k)) {
                    uint32_t ret = c.r[14];
                    ARM_POLL(c);
                    f(c);
                    if (link && c.pc == ret) { k = c.pc; break; }    // back from the call: go on here
                    k = c.pc;
                    link = false;
                    by_reg = true;
                    continue;
                }
                if (!link && by_reg && !from_program && in_program(k)) { c.pc = k; return false; }
            }
            break;
        }
        if (link || (k & ~1u) <= pc) ARM_POLL(c);
        pc = k & ~1u;
        t = k & 1;
        return true;
    }

    void run(uint32_t start) {
        from_program = in_program(start);
        pc = start & ~1u;
        t = start & 1;
        if (!t) pc &= ~3u;
        for (;;) {
            flow = NEXT;
            uint32_t size;
            if (t) size = thumb(ld16(pc));
            else { arm(ld32(pc)); size = 4; }
            switch (flow) {
            case NEXT: pc += size; break;
            case JUMP: if (!transfer(to, false, reg)) return; break;
            case CALL: if (!transfer(to, true, true)) return; break;
            case ERET: if (!transfer(c.pc, false, true)) return; break;
            }
        }
    }
};

}  // namespace

void arm_interpret(ArmContext& c, uint32_t key) {
    if (!g_arm_interp_only) arm_interpreting(c, key);
    if (c.cpu == 9) {
        Interp<9> it{c, c.r[14]};
        it.run(key);
    } else {
        Interp<7> it{c, c.r[14]};
        it.run(key);
    }
}
