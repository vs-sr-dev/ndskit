// ndskit runtime — the part every build shares: the memories the CPUs reach
// directly, the processor modes and their banked registers, and the
// key -> function dispatch over the active modules (one per CPU).
#include "arm.h"
#include <cstdio>
#include <cstring>
#include <vector>

alignas(64) uint8_t g_main[0x400000 + 8];      // +8: a word read at the last byte stays inside
alignas(64) uint8_t g_itcm[0x8000 + 8];
alignas(64) uint8_t g_dtcm[0x4000 + 8];
alignas(64) uint8_t g_wram7[0x10000 + 8];
alignas(64) uint8_t g_swram[0x8000 + 8];
uint32_t g_dtcm_base = 0x027E0000u;            // where the SDK's crt0 puts it (CP15 c9,c1,0)

// ---- the address map outside the fast paths ------------------------------------------
uint32_t nds_io_read(int cpu, uint32_t a, int size) { return nds_mmio_read(cpu, a, size); }
void nds_io_write(int cpu, uint32_t a, uint32_t v, int size) { nds_mmio_write(cpu, a, v, size); }

static uint8_t* host(int cpu, uint32_t a) {
    return cpu == 9 ? nds_fast<9>(a) : nds_fast<7>(a);
}

bool nds_mem_write(int cpu, uint32_t addr, const void* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = host(cpu, addr + (uint32_t)i);
        if (!p) return false;
        *p = s[i];
    }
    return true;
}

bool nds_mem_read(int cpu, uint32_t addr, void* dst, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = host(cpu, addr + (uint32_t)i);
        if (!p) return false;
        d[i] = *p;
    }
    return true;
}

// ---- modes ---------------------------------------------------------------------------
static int bank_of(uint32_t mode) {
    switch (mode) {
    case 0x11: return ARM_BANK_FIQ;
    case 0x12: return ARM_BANK_IRQ;
    case 0x13: return ARM_BANK_SVC;
    case 0x17: return ARM_BANK_ABT;
    case 0x1B: return ARM_BANK_UND;
    default: return ARM_BANK_USR;              // user, system (and the reserved modes)
    }
}

void arm_set_mode(ArmContext& c, uint32_t mode) {
    int ob = bank_of(c.mode), nb = bank_of(mode);
    if (ob != nb) {
        c.bank[ob][0] = c.r[13]; c.bank[ob][1] = c.r[14];
        if ((ob == ARM_BANK_FIQ) != (nb == ARM_BANK_FIQ))
            for (int i = 0; i < 5; ++i) { uint32_t t = c.r[8 + i]; c.r[8 + i] = c.fiq_other[i]; c.fiq_other[i] = t; }
        c.r[13] = c.bank[nb][0]; c.r[14] = c.bank[nb][1];
    }
    c.mode = mode;
}

uint32_t arm_get_spsr(const ArmContext& c) {
    int b = bank_of(c.mode);
    return b == ARM_BANK_USR ? arm_get_cpsr(c) : c.spsr[b];
}

void arm_set_spsr(ArmContext& c, uint32_t v, uint32_t mask) {
    int b = bank_of(c.mode);
    if (b == ARM_BANK_USR) return;
    uint32_t m = (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) |
                 (mask & 8 ? 0xFF000000u : 0);
    c.spsr[b] = (c.spsr[b] & ~m) | (v & m);
}

void arm_exception_return(ArmContext& c, uint32_t target) {
    uint32_t t = 0;
    if (bank_of(c.mode) != ARM_BANK_USR) {
        uint32_t s = c.spsr[bank_of(c.mode)];
        arm_set_cpsr(c, s, 0xF);
        t = s >> 5 & 1;
    }
    c.pc = t ? (target & ~1u) | 1u : target & ~3u;
}

uint32_t arm_user_reg(const ArmContext& c, int n) {
    int b = bank_of(c.mode);
    if (n >= 13 && b != ARM_BANK_USR) return c.bank[ARM_BANK_USR][n - 13];
    if (n >= 8 && n <= 12 && b == ARM_BANK_FIQ) return c.fiq_other[n - 8];
    return c.r[n];
}

void arm_set_user_reg(ArmContext& c, int n, uint32_t v) {
    int b = bank_of(c.mode);
    if (n >= 13 && b != ARM_BANK_USR) c.bank[ARM_BANK_USR][n - 13] = v;
    else if (n >= 8 && n <= 12 && b == ARM_BANK_FIQ) c.fiq_other[n - 8] = v;
    else c.r[n] = v;
}

// ---- modules ---------------------------------------------------------------------------
uint32_t nds_crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

const ArmModule* arm_module(const char* name) {
    for (int i = 0; i < g_arm_nmodules; ++i)
        if (!std::strcmp(g_arm_modules[i]->name, name)) return g_arm_modules[i];
    return nullptr;
}

bool arm_identify(const ArmModule* m) {
    for (uint32_t s = 0; s < m->nsegs; ++s) {
        const ArmSegment& g = m->segs[s];
        std::vector<uint8_t> buf(g.size);          // a segment may span two memories (the ARM7's WRAMs)
        if (!nds_mem_read(m->cpu, g.base, buf.data(), g.size)) return false;
        if (nds_crc32(buf.data(), g.size) != g.crc) return false;
    }
    return true;
}

static const ArmModule* g_active[2];             // [0] the ARM9's, [1] the ARM7's

void arm_activate(const ArmModule* m) { g_active[m->cpu == 9 ? 0 : 1] = m; }

ArmFunc arm_lookup(int cpu, uint32_t key) {
    const ArmModule* m = g_active[cpu == 9 ? 0 : 1];
    if (!m) return nullptr;
    uint32_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].key < key) lo = mid + 1; else hi = mid;
    }
    if (lo < m->nfuncs && m->funcs[lo].key == key) return m->funcs[lo].fn;
    return nullptr;
}

void arm_call(ArmContext& c, uint32_t key) {
    if (ArmFunc f = arm_lookup(c.cpu, key)) f(c);
    else arm_call_unknown(c, key);
}

// A jump through a register whose target analysis did not know: an entry is
// a tail call; anything else is taken for a return (a copy of lr, a saved
// return address) and checked by the caller's ARM_RET.
void arm_jump(ArmContext& c, uint32_t key) {
    if (ArmFunc f = arm_lookup(c.cpu, key)) f(c);
    else c.pc = key;
}
