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
        uint32_t a = addr + (uint32_t)i;
        uint8_t* p = host(cpu, a);
        if (!p) return false;
        cpu == 9 ? nds_ovl_store<9>(a) : nds_ovl_store<7>(a);
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
        if (!host(m->cpu, g.base)) continue;       // not memory the core holds (VRAM): not checked
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
// Each CPU's overlays' area (from the lowest load address to the highest end)
// in pages of 256 bytes. Every overlay function of the build is listed by key
// with its state: unknown (to check), its bytes in memory as built, or not. A
// store into a page (nds_ovl_store) makes the functions over it unknown again;
// a function is checked (the crc32 of its extent) when a call reaches it.
uint32_t g_ovl_lo[2], g_ovl_len[2];
uint8_t* g_ovl_dirty[2];
uint8_t g_ovl_any[2];

namespace {
enum : uint8_t { UNKNOWN, THERE, GONE };
struct OvFn { uint32_t key; const ArmModule* m; uint32_t i; };
struct Area {
    uint32_t lo = 0, hi = 0;
    std::vector<OvFn> fns;                       // sorted by key
    std::vector<uint8_t> state;                  // of each of fns
    std::vector<std::vector<uint32_t>> on_page;  // the fns whose extent meets each page
    std::vector<uint8_t> dirty;
    std::vector<const ArmModule*> found;         // modules with a function found since their last miss
};
Area g_area[2];
bool g_areas_made;

void make_areas() {
    g_areas_made = true;
    for (int ci = 0; ci < 2; ++ci) {
        Area& A = g_area[ci];
        int cpu = ci ? 7 : 9;
        bool any = false;
        for (int k = 0; k < g_arm_nmodules; ++k) {
            const ArmModule* m = g_arm_modules[k];
            if (m->cpu != cpu || m->overlay < 0 || !m->nsegs) continue;
            const ArmSegment& s = m->segs[0];
            if (!any || s.base < A.lo) A.lo = s.base;
            if (!any || s.base + s.size > A.hi) A.hi = s.base + s.size;
            any = true;
            for (uint32_t i = 0; i < m->nfuncs; ++i) A.fns.push_back({m->funcs[i].key, m, i});
        }
        if (!any) continue;
        A.lo &= ~0xFFu;
        std::stable_sort(A.fns.begin(), A.fns.end(), [](const OvFn& a, const OvFn& b) { return a.key < b.key; });
        A.state.assign(A.fns.size(), UNKNOWN);
        uint32_t pages = (A.hi - A.lo + 255) >> 8;
        A.on_page.resize(pages);
        A.dirty.assign(pages, 0);
        for (uint32_t j = 0; j < A.fns.size(); ++j) {
            const ArmRange& e = A.fns[j].m->extents[A.fns[j].i];
            if (!e.size) continue;
            for (uint32_t p = (e.base - A.lo) >> 8; p <= (e.base + e.size - 1 - A.lo) >> 8 && p < pages; ++p)
                A.on_page[p].push_back(j);
        }
        g_ovl_dirty[ci] = A.dirty.data();
        g_ovl_lo[ci] = A.lo;
        g_ovl_len[ci] = pages << 8;
    }
}

void forget_written(int ci) {
    Area& A = g_area[ci];
    g_ovl_any[ci] = 0;
    for (size_t p = 0; p < A.dirty.size(); ++p)
        if (A.dirty[p]) {
            A.dirty[p] = 0;
            for (uint32_t j : A.on_page[p]) A.state[j] = UNKNOWN;
        }
}

bool bytes_there(int cpu, const ArmModule* m, uint32_t i) {
    const ArmRange& e = m->extents[i];
    uint8_t buf[4096];
    uint32_t crc = 0;
    for (uint32_t o = 0; o < e.size; o += sizeof buf) {
        uint32_t n = std::min<uint32_t>(sizeof buf, e.size - o);
        if (!nds_mem_read(cpu, e.base + o, buf, n)) return false;
        crc = nds_crc32(buf, n, crc);
    }
    return crc == m->crcs[i];
}

// The overlay function at key whose bytes are in memory, or -1 (an index of fns).
int overlay_fn(int cpu, uint32_t key) {
    if (!g_areas_made) make_areas();
    int ci = cpu == 9 ? 0 : 1;
    Area& A = g_area[ci];
    if ((key & ~1u) - A.lo >= A.hi - A.lo) return -1;
    if (g_ovl_any[ci]) forget_written(ci);
    auto it = std::lower_bound(A.fns.begin(), A.fns.end(), key, [](const OvFn& f, uint32_t k) { return f.key < k; });
    for (; it != A.fns.end() && it->key == key; ++it) {
        uint32_t j = (uint32_t)(it - A.fns.begin());
        if (A.state[j] == UNKNOWN) {
            bool there = bytes_there(cpu, it->m, it->i);
            A.state[j] = there ? THERE : GONE;
            auto f = std::find(A.found.begin(), A.found.end(), it->m);
            if (there && f == A.found.end()) {
                A.found.push_back(it->m);
                arm_overlay_found(cpu, it->m);
            } else if (!there && f != A.found.end()) {
                A.found.erase(f);
            }
        }
        if (A.state[j] == THERE) return (int)j;
    }
    return -1;
}
}  // namespace

ArmFunc arm_lookup(int cpu, uint32_t key) {
    const ArmModule* m = g_active[cpu == 9 ? 0 : 1];
    if (!m) return nullptr;
    int i = find_func(m, key);
    if (i >= 0) return m->funcs[i].fn;
    int j = overlay_fn(cpu, key);
    if (j < 0) return nullptr;
    const OvFn& f = g_area[cpu == 9 ? 0 : 1].fns[j];
    return f.m->funcs[f.i].fn;
}

void arm_overlays_unknown() {
    for (Area& A : g_area) std::fill(A.state.begin(), A.state.end(), UNKNOWN);
}

static bool has_landing(const ArmModule* m, uint32_t key) {
    if (!m || !m->nlandings) return false;
    const uint32_t* e = m->landings + m->nlandings;
    const uint32_t* p = std::lower_bound(m->landings, e, key);
    return p != e && *p == key;
}

bool arm_is_landing(int cpu, uint32_t key) {
    if (has_landing(g_active[cpu == 9 ? 0 : 1], key)) return true;
    if (!g_areas_made) make_areas();
    for (const ArmModule* m : g_area[cpu == 9 ? 0 : 1].found)     // a landing in an overlay found in memory
        if ((key & ~1u) - m->segs[0].base < m->segs[0].size && has_landing(m, key)) return true;
    return false;
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
