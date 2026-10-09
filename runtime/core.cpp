// ndskit runtime — the part every build shares: the memories the CPUs reach
// directly, the processor modes and their banked registers, and the
// key -> function dispatch over the active modules (one per CPU).
#include "arm.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

alignas(64) uint8_t g_main[0x400000 + 8];      // +8: a word read at the last byte stays inside
alignas(64) uint8_t g_itcm[0x8000 + 8];
alignas(64) uint8_t g_dtcm[0x4000 + 8];
alignas(64) uint8_t g_wram7[0x10000 + 8];
alignas(64) uint8_t g_swram[0x8000 + 8];
uint32_t g_dtcm_base = 0x027E0000u;            // where the SDK's crt0 puts it (CP15 c9,c1,0)
uint32_t g_dtcm_size = 0x4000u;
uint32_t g_watch_lo, g_watch_len;              // no watch                // on (the runtime follows CP15's control register)

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

void arm_restore_cpsr(ArmContext& c, uint32_t target) {
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
        if (g.kind != ARM_SEG_PROGRAM) continue;   // not there until the program copies or loads it
        std::vector<uint8_t> buf(g.size);          // a segment may span two memories (the ARM7's WRAMs)
        if (!nds_mem_read(m->cpu, g.base, buf.data(), g.size)) return false;
        if (nds_crc32(buf.data(), g.size) != g.crc) return false;
    }
    return true;
}

static const ArmModule* g_active[2];             // [0] the ARM9's, [1] the ARM7's

void arm_activate(const ArmModule* m) { g_active[m->cpu == 9 ? 0 : 1] = m; }

static int find_func(const ArmModule* m, uint32_t key) {
    uint32_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo < m->nfuncs && m->funcs[lo].key == key ? (int)lo : -1;
}

// ---- overlays ---------------------------------------------------------------------------
uint32_t g_arm_overlay_epoch = 1;

namespace {
struct Slot {                                    // one load address and the overlays that share it
    int cpu;
    uint32_t base, end;                          // the largest of them
    std::vector<const ArmModule*> cands;
    const ArmModule* resident = nullptr;
    uint32_t epoch = 0;                          // when resident was last checked
};
std::vector<Slot> g_slots;
bool g_slots_made;

void make_slots() {
    g_slots_made = true;
    for (int i = 0; i < g_arm_nmodules; ++i) {
        const ArmModule* m = g_arm_modules[i];
        if (m->overlay < 0 || !m->nsegs) continue;
        const ArmSegment& s = m->segs[0];
        Slot* t = nullptr;
        for (Slot& x : g_slots)
            if (x.cpu == m->cpu && x.base == s.base) t = &x;
        if (!t) { g_slots.push_back(Slot{m->cpu, s.base, s.base}); t = &g_slots.back(); }
        t->end = std::max(t->end, s.base + s.size);
        t->cands.push_back(m);
    }
}

bool code_matches(const ArmModule* m) {
    uint32_t crc = 0;
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < m->ncode; ++i) {
        buf.resize(m->code[i].size);
        if (!nds_mem_read(m->cpu, m->code[i].base, buf.data(), buf.size())) return false;
        crc = nds_crc32(buf.data(), buf.size(), crc);
    }
    return crc == m->code_crc;
}

bool samples_match(const ArmModule* m) {
    for (uint32_t i = 0; i < m->nsamples; ++i) {
        uint32_t w = 0;
        if (!nds_mem_read(m->cpu, m->samples[2 * i], &w, 4) || w != m->samples[2 * i + 1]) return false;
    }
    return true;
}

// Which overlay the slot holds. `cheap`: memory may have changed (the cartridge
// delivered data) and the one resident is checked by its samples first; a full
// check (every candidate's code crc32) when it is not there, or none was.
void check_slot(Slot& s, bool cheap = false) {
    const ArmModule* was = s.resident;
    const ArmModule* now = nullptr;
    if (was && cheap && samples_match(was)) {
        s.epoch = g_arm_overlay_epoch;
        return;
    }
    if (was && code_matches(was)) now = was;     // most often still there
    for (size_t i = 0; !now && i < s.cands.size(); ++i)
        if (s.cands[i] != was && code_matches(s.cands[i])) now = s.cands[i];
    s.resident = now;
    s.epoch = g_arm_overlay_epoch;
    if (now != was) arm_overlay_changed(s.cpu, s.base, now);
}

Slot* slot_of(int cpu, uint32_t a) {
    if (!g_slots_made) make_slots();
    for (Slot& s : g_slots)
        if (s.cpu == cpu && a - s.base < s.end - s.base) return &s;
    return nullptr;
}

uint32_t first_insn(int cpu, uint32_t key) {
    uint32_t w = 0;
    nds_mem_read(cpu, key & ~1u, &w, key & 1 ? 2 : 4);
    return w;
}
}  // namespace

const ArmModule* arm_overlay_resident(int cpu, uint32_t addr) {
    Slot* s = slot_of(cpu, addr);
    if (!s) return nullptr;
    if (s->epoch != g_arm_overlay_epoch) check_slot(*s, true);
    return s->resident;
}

ArmFunc arm_lookup(int cpu, uint32_t key) {
    const ArmModule* m = g_active[cpu == 9 ? 0 : 1];
    if (!m) return nullptr;
    int i = find_func(m, key);
    if (i >= 0) return m->funcs[i].fn;
    Slot* s = slot_of(cpu, key & ~1u);
    if (!s) return nullptr;
    if (s->epoch != g_arm_overlay_epoch) check_slot(*s, true);
    for (int pass = 0; pass < 2 && s->resident; ++pass) {
        const ArmModule* o = s->resident;
        i = find_func(o, key);
        if (i >= 0 && first_insn(cpu, key) == o->firsts[i]) return o->funcs[i].fn;
        if (pass) break;
        check_slot(*s);                          // not this overlay's entry or bytes: which one is there?
        if (s->resident == o) break;
    }
    return nullptr;
}

bool arm_is_landing(int cpu, uint32_t key) {
    const ArmModule* m = arm_overlay_resident(cpu, key & ~1u);    // a landing in an overlay
    if (!m) m = g_active[cpu == 9 ? 0 : 1];
    if (!m || !m->nlandings) return false;
    const uint32_t* e = m->landings + m->nlandings;
    const uint32_t* p = std::lower_bound(m->landings, e, key);
    return p != e && *p == key;
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
