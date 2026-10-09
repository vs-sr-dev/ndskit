// ndskit runtime — the ARM9 and the ARM7 as seen by recompiled code.
//
// Recompiled functions have the signature `void f_XXXXXXXX(ArmContext& c)`
// (ARM code) or `t_XXXXXXXX` (Thumb), one namespace per program. They touch
// guest state only through this header: registers and flags in ArmContext,
// memory through ld*/st* (little-endian, as the host: main RAM, the TCMs and
// the ARM7's WRAM as host arrays, everything else through nds_io_*), and a
// few out-of-line services (calls through registers, safe points, modes,
// CP15, SWI).
//
// Semantics are ndskit/armemu.py's, instruction for instruction: the
// self-test (runtime/selftest.cpp) holds the two together.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#define ARM_UNLIKELY(x) __builtin_expect(!!(x), 0)

// Banks of r13/r14 and the SPSR: user/system, FIQ, IRQ, supervisor, abort, undefined.
enum { ARM_BANK_USR, ARM_BANK_FIQ, ARM_BANK_IRQ, ARM_BANK_SVC, ARM_BANK_ABT, ARM_BANK_UND, ARM_NBANKS };

struct ArmContext {
    uint32_t r[16];                     // r[15] unused: recompiled code knows its pc
    uint32_t n, z, c, v, q;             // the CPSR's flags, one field each
    uint32_t i, f, mode;                // its control bits (T is the function's own)
    uint32_t pc;                        // where the last return or jump went (bit 0: Thumb)
    int32_t  budget;                    // safe points left before arm_poll
    int      cpu;                       // 9 or 7
    uint32_t bank[ARM_NBANKS][2];       // r13, r14 of the banks not in use
    uint32_t fiq_other[5];              // r8-r12 of the other side of FIQ
    uint32_t spsr[ARM_NBANKS];
    uint32_t irq;                       // an interrupt is pending (IME, IE & IF): the runtime keeps it
};

typedef void (*ArmFunc)(ArmContext&);

// A program's recompiled code: its segments as they lie in memory where
// they run (to recognise them), and its entries sorted by key (the address,
// bit 0 set for Thumb).
struct ArmFuncEntry { uint32_t key; ArmFunc fn; };
enum { ARM_SEG_PROGRAM = 0, ARM_SEG_COPIED = 1, ARM_SEG_OVERLAY = 2 };
struct ArmSegment { uint32_t base, size, crc, kind; };    // kind: ARM_SEG_*; copied: put there at run time
struct ArmRange { uint32_t base, size; };
struct ArmModule {
    const char* name;
    int cpu;
    const ArmSegment* segs;
    uint32_t nsegs;
    const ArmFuncEntry* funcs;
    uint32_t nfuncs;
    const uint32_t* landings;           // return addresses of calls to a context save (sorted keys)
    uint32_t nlandings;
    // An overlay's module (overlay >= 0; its one segment is where it loads, shared
    // with other overlays): resident when the bytes of its code (the ranges, data
    // left out: it changes once loaded) have code_crc; firsts[i] is the first
    // instruction of funcs[i], checked at each call dispatched into it.
    int overlay;
    const ArmRange* code;
    uint32_t ncode;
    uint32_t code_crc;
    const uint32_t* firsts;
};

// A return that went to a landing of the active module, not to the call's
// own address: a saved context was loaded. The runtime throws it; the
// functions holding the landing catch it around their calls and go on there.
struct ArmUnwind { uint32_t pc; };

// ---- memory -------------------------------------------------------------------------
extern uint8_t g_main[];                // 4 MiB at 0x02000000, repeated to 0x02FFFFFF
extern uint8_t g_itcm[];                // ARM9: 32 KiB, repeated from 0 to 0x01FFFFFF
extern uint8_t g_dtcm[];                // ARM9: 16 KiB at g_dtcm_base, over everything else
extern uint8_t g_wram7[];               // ARM7: 64 KiB at 0x03800000, repeated to 0x03FFFFFF
extern uint8_t g_swram[];               // shared WRAM, 32 KiB: the ARM7's over 0x03000000-0x037FFFFF
                                        // (WRAMCNT = 3, as the SDK's boot leaves it)
extern uint32_t g_dtcm_base;
extern uint32_t g_dtcm_size;            // 0x4000 while CP15 enables the DTCM, else 0
// A store watch (a debugging aid): every store into [g_watch_lo, +g_watch_len) is reported.
extern uint32_t g_watch_lo, g_watch_len;
void nds_watch_hit(int cpu, uint32_t a, uint32_t v, int size);
uint32_t nds_io_read(int cpu, uint32_t a, int size);    // everything else (core.cpp)
void     nds_io_write(int cpu, uint32_t a, uint32_t v, int size);
uint32_t nds_mmio_read(int cpu, uint32_t a, int size);  // what nds_io_* does not hold: the hardware
void     nds_mmio_write(int cpu, uint32_t a, uint32_t v, int size);

template <int CPU> static inline uint8_t* nds_fast(uint32_t a) {
    if (CPU == 9) {
        if (a - g_dtcm_base < g_dtcm_size) return g_dtcm + (a - g_dtcm_base);
        if (a < 0x02000000u) return g_itcm + (a & 0x7FFFu);
    }
    if (a >> 24 == 0x02) return g_main + (a & 0x3FFFFFu);
    if (CPU == 7 && a >> 23 == 7) return g_wram7 + (a & 0xFFFFu);
    if (CPU == 7 && a >> 23 == 6) return g_swram + (a & 0x7FFFu);
    return nullptr;
}

// Aligned accesses (callers align: words to 4, halfwords to 2).
template <int CPU> static inline uint32_t nds_ld8(uint32_t a) {
    if (uint8_t* p = nds_fast<CPU>(a)) return *p;
    return nds_io_read(CPU, a, 1) & 0xFFu;
}
template <int CPU> static inline uint32_t nds_ld16(uint32_t a) {
    uint16_t v;
    if (uint8_t* p = nds_fast<CPU>(a)) { std::memcpy(&v, p, 2); return v; }
    return nds_io_read(CPU, a, 2) & 0xFFFFu;
}
template <int CPU> static inline uint32_t nds_ld32(uint32_t a) {
    uint32_t v;
    if (uint8_t* p = nds_fast<CPU>(a)) { std::memcpy(&v, p, 4); return v; }
    return nds_io_read(CPU, a, 4);
}
template <int CPU> static inline void nds_st8(uint32_t a, uint32_t v) {
    if (ARM_UNLIKELY(a - g_watch_lo < g_watch_len)) nds_watch_hit(CPU, a, v & 0xFFu, 1);
    if (uint8_t* p = nds_fast<CPU>(a)) *p = (uint8_t)v;
    else nds_io_write(CPU, a, v & 0xFFu, 1);
}
template <int CPU> static inline void nds_st16(uint32_t a, uint32_t v) {
    if (ARM_UNLIKELY(a - g_watch_lo < g_watch_len)) nds_watch_hit(CPU, a, v & 0xFFFFu, 2);
    uint16_t w = (uint16_t)v;
    if (uint8_t* p = nds_fast<CPU>(a)) std::memcpy(p, &w, 2);
    else nds_io_write(CPU, a, v & 0xFFFFu, 2);
}
template <int CPU> static inline void nds_st32(uint32_t a, uint32_t v) {
    if (ARM_UNLIKELY(a - g_watch_lo + 3 < g_watch_len + 3)) nds_watch_hit(CPU, a, v, 4);
    if (uint8_t* p = nds_fast<CPU>(a)) std::memcpy(p, &v, 4);
    else nds_io_write(CPU, a, v, 4);
}

static inline uint32_t arm_ror(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

// What the generated code of each CPU calls: `using namespace nds9;` (or 7).
// ldr32 is a word load at any address (the aligned word, rotated); ldrh and
// ldrsh at an odd address read the aligned halfword on the ARM9, and on the
// ARM7 rotate it (ldrh) or read the signed byte (ldrsh).
#define NDS_CPU_ACCESSORS(CPU)                                                                       \
    namespace nds##CPU {                                                                             \
    static inline uint32_t ld8(uint32_t a) { return nds_ld8<CPU>(a); }                               \
    static inline uint32_t ld16(uint32_t a) { return nds_ld16<CPU>(a); }                             \
    static inline uint32_t ld32(uint32_t a) { return nds_ld32<CPU>(a); }                             \
    static inline void st8(uint32_t a, uint32_t v) { nds_st8<CPU>(a, v); }                           \
    static inline void st16(uint32_t a, uint32_t v) { nds_st16<CPU>(a, v); }                         \
    static inline void st32(uint32_t a, uint32_t v) { nds_st32<CPU>(a, v); }                         \
    static inline uint32_t ldr32(uint32_t a) { return arm_ror(nds_ld32<CPU>(a & ~3u), (a & 3u) * 8); } \
    static inline uint32_t ldrh(uint32_t a) {                                                        \
        if (CPU == 9 || !(a & 1)) return nds_ld16<CPU>(a & ~1u);                                     \
        return arm_ror(nds_ld16<CPU>(a & ~1u), 8);                                                   \
    }                                                                                                \
    static inline uint32_t ldrsh(uint32_t a) {                                                       \
        if (CPU == 9 || !(a & 1)) return (uint32_t)(int16_t)nds_ld16<CPU>(a & ~1u);                 \
        return (uint32_t)(int8_t)nds_ld8<CPU>(a);                                                    \
    }                                                                                                \
    }
NDS_CPU_ACCESSORS(9)
NDS_CPU_ACCESSORS(7)

// Guest memory as bytes, for loaders and tests (main RAM, the TCMs, the ARM7's WRAMs).
bool nds_mem_write(int cpu, uint32_t addr, const void* src, size_t n);
bool nds_mem_read(int cpu, uint32_t addr, void* dst, size_t n);

// ---- the shifter -------------------------------------------------------------------
// By a register: the amount is the bottom byte of Rs (0-255).
static inline uint32_t arm_lsl(uint32_t v, uint32_t n) { return n < 32 ? v << n : 0; }
static inline uint32_t arm_lsr(uint32_t v, uint32_t n) { return n < 32 ? v >> n : 0; }
static inline uint32_t arm_asr(uint32_t v, uint32_t n) { return (uint32_t)((int32_t)v >> (n < 32 ? n : 31)); }
// ... and the carry out, into cy (left alone for an amount of 0)
static inline uint32_t arm_lsl_c(uint32_t v, uint32_t n, uint32_t& cy) {
    if (n == 0) return v;
    if (n < 32) { cy = v >> (32 - n) & 1; return v << n; }
    cy = n == 32 ? v & 1 : 0;
    return 0;
}
static inline uint32_t arm_lsr_c(uint32_t v, uint32_t n, uint32_t& cy) {
    if (n == 0) return v;
    if (n < 32) { cy = v >> (n - 1) & 1; return v >> n; }
    cy = n == 32 ? v >> 31 : 0;
    return 0;
}
static inline uint32_t arm_asr_c(uint32_t v, uint32_t n, uint32_t& cy) {
    if (n == 0) return v;
    if (n < 32) { cy = v >> (n - 1) & 1; return (uint32_t)((int32_t)v >> n); }
    cy = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t arm_ror_c(uint32_t v, uint32_t n, uint32_t& cy) {
    if (n == 0) return v;
    v = arm_ror(v, n);
    cy = v >> 31;
    return v;
}

// ---- saturating arithmetic (ARMv5TE) -------------------------------------------------
static inline uint32_t arm_sat(ArmContext& c, int64_t x) {
    if (x > INT32_MAX) { c.q = 1; return (uint32_t)INT32_MAX; }
    if (x < INT32_MIN) { c.q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)x;
}
static inline uint32_t arm_qadd(ArmContext& c, uint32_t m, uint32_t n) {
    return arm_sat(c, (int64_t)(int32_t)m + (int32_t)n);
}
static inline uint32_t arm_qsub(ArmContext& c, uint32_t m, uint32_t n) {
    return arm_sat(c, (int64_t)(int32_t)m - (int32_t)n);
}
static inline uint32_t arm_qdadd(ArmContext& c, uint32_t m, uint32_t n) {
    return arm_sat(c, (int64_t)(int32_t)m + (int32_t)arm_sat(c, 2 * (int64_t)(int32_t)n));
}
static inline uint32_t arm_qdsub(ArmContext& c, uint32_t m, uint32_t n) {
    return arm_sat(c, (int64_t)(int32_t)m - (int32_t)arm_sat(c, 2 * (int64_t)(int32_t)n));
}

// ---- the status registers ----------------------------------------------------------
static inline uint32_t arm_get_cpsr(const ArmContext& c) {
    return c.n << 31 | c.z << 30 | c.c << 29 | c.v << 28 | c.q << 27 | c.i << 7 | c.f << 6 | c.mode;
}
void arm_set_mode(ArmContext& c, uint32_t mode);         // switches the banked registers (core.cpp)
// msr cpsr_<fields>: mask c=1, x=2, s=4, f=8; in user mode only the flags change.
static inline void arm_set_cpsr(ArmContext& c, uint32_t v, uint32_t mask) {
    if (mask & 8) {
        c.n = v >> 31; c.z = v >> 30 & 1; c.c = v >> 29 & 1; c.v = v >> 28 & 1;
        if (c.cpu == 9) c.q = v >> 27 & 1;
    }
    if (mask & 1 && c.mode != 0x10) {
        c.i = v >> 7 & 1; c.f = v >> 6 & 1;
        arm_set_mode(c, (v & 0x1F) | 0x10);
        if (c.irq && !c.i) c.budget = 0;    // unmasked with one pending: taken at the next safe point
    }
}
static inline void arm_set_nzcv(ArmContext& c, uint32_t v) {
    c.n = v >> 31; c.z = v >> 30 & 1; c.c = v >> 29 & 1; c.v = v >> 28 & 1;
}
uint32_t arm_get_spsr(const ArmContext& c);              // the CPSR in user and system mode
void     arm_set_spsr(ArmContext& c, uint32_t v, uint32_t mask);
// subs pc, lr, #n / ldm ..., {..., pc}^: CPSR = SPSR, then c.pc = target in the restored state
// (arm_restore_cpsr); the services decide where that goes (another thread's context)
void     arm_restore_cpsr(ArmContext& c, uint32_t target);
void     arm_exception_return(ArmContext& c, uint32_t target);
uint32_t arm_user_reg(const ArmContext& c, int n);       // ldm/stm with ^
void     arm_set_user_reg(ArmContext& c, int n, uint32_t v);

// The address a bx/blx (or, on the ARMv5, a load into pc) goes to: bit 0 the mode.
static inline uint32_t arm_iw(uint32_t x) { return x & 1u ? x : x & ~3u; }

// ---- services provided by the runtime ------------------------------------------------
void     arm_call(ArmContext& c, uint32_t key);          // a call through a register: an entry, or not
void     arm_jump(ArmContext& c, uint32_t key);          // a jump: an entry is called; elsewhere is a return
void     arm_poll(ArmContext& c);                        // the budget is spent: time, interrupts
void     arm_swi(ArmContext& c, uint32_t num, uint32_t ret);
void     arm_undefined(ArmContext& c, uint32_t pc);
uint32_t arm_cp15_read(ArmContext& c, int op1, int crn, int crm, int op2);
void     arm_cp15_write(ArmContext& c, int op1, int crn, int crm, int op2, uint32_t v);
void     arm_bad_return(ArmContext& c, uint32_t expected);   // a return went elsewhere
void     arm_call_unknown(ArmContext& c, uint32_t key);  // services: a call to a non-entry
void     arm_context_saved(ArmContext& c);               // a context save returned (its caller's sp)

// A safe point: backward jumps, calls, and an msr that may unmask interrupts.
#define ARM_POLL(c) do { if (ARM_UNLIKELY(--(c).budget < 0)) arm_poll(c); } while (0)
// At each function's entry: when the runtime traces calls (a debugging aid), it
// is told; otherwise one predictable branch.
extern int g_arm_trace;
void arm_trace_enter(ArmContext& c, uint32_t key);
#define ARM_ENTER(c, k) do { if (ARM_UNLIKELY(g_arm_trace)) arm_trace_enter(c, k); } while (0)
// After a call: the callee's return must have come back here.
#define ARM_RET(c, a) do { if (ARM_UNLIKELY((c).pc != (a))) arm_bad_return(c, a); } while (0)

// ---- the runtime's view of the modules (core.cpp) --------------------------------------
// The generated modules.cpp lists every module of the build; one module per
// CPU is active, the one whose segments arm_identify finds in memory.
extern const ArmModule* const g_arm_modules[];
extern const int g_arm_nmodules;
const ArmModule* arm_module(const char* name);
bool    arm_identify(const ArmModule* m);                // its segments are in memory, as built
void    arm_activate(const ArmModule* m);                // the one module of its CPU
ArmFunc arm_lookup(int cpu, uint32_t key);               // nullptr if not an entry
bool    arm_is_landing(int cpu, uint32_t key);           // a landing of the active module
uint32_t nds_crc32(const uint8_t* p, size_t n, uint32_t crc = 0);
// Overlays: an address in an overlay's load area is looked up in the module of the
// overlay resident there, found by its code's crc32 and checked again whenever
// memory may have changed under it (the epoch: the cartridge bumps it with every
// word it delivers) or a dispatched call's first instruction is not the module's.
extern uint32_t g_arm_overlay_epoch;
const ArmModule* arm_overlay_resident(int cpu, uint32_t addr);  // nullptr: none of the build's
void    arm_overlay_changed(int cpu, uint32_t base, const ArmModule* now);  // services: a slot changed hands
