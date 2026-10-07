// ndskit runtime — timers, DMA, IPC and the ARM9's maths unit.
//
// Timers count on the system clock (2 ticks) through their prescaler; their
// counters are worked out from the time when read, and an event at the next
// overflow raises the interrupt and feeds a count-up timer. DMA copies a
// whole transfer at once, when started (immediate, and the geometry FIFO's,
// which never fills here), at VBlank or HBlank, or for each word the card
// has ready. The IPC registers are the two CPUs' SYNC nibbles and two FIFOs
// of 16 words, with their interrupts. The maths unit answers at once, with
// the results melonDS gives (division by zero included).
#include "nds.h"
#include <algorithm>
#include <cstring>
#include <deque>

static int ci(int cpu) { return cpu == 9 ? 0 : 1; }

// ---- timers ---------------------------------------------------------------------------------
struct Timer { uint16_t reload, ctl, counter; uint64_t base; };
static Timer g_tm[2][4];
static const int kShift[4] = {0, 6, 8, 10};

static uint64_t period(const Timer& t) { return 2ull << kShift[t.ctl & 3]; }
static bool free_running(int n, const Timer& t) { return (t.ctl & 0x80) && !(n > 0 && (t.ctl & 4)); }

static void schedule_timer(int cpu, int n);

// The counter moves on by `counts`; overflows raise the interrupt and feed the next timer.
static void advance(int cpu, int n, uint64_t counts) {
    Timer& t = g_tm[ci(cpu)][n];
    uint64_t v = t.counter + counts;
    if (v < 0x10000) { t.counter = (uint16_t)v; return; }
    uint64_t rest = v - 0x10000, span = 0x10000 - t.reload;
    uint64_t ov = 1 + rest / span;
    t.counter = (uint16_t)(t.reload + rest % span);
    if (t.ctl & 0x40) nds_irq_raise(cpu, 3 + n);
    if (n < 3) {
        Timer& u = g_tm[ci(cpu)][n + 1];
        if ((u.ctl & 0x84) == 0x84) advance(cpu, n + 1, ov);
    }
}

static void update(int cpu, int n, uint64_t now) {
    Timer& t = g_tm[ci(cpu)][n];
    if (!free_running(n, t) || now <= t.base) return;
    uint64_t p = period(t), counts = (now - t.base) / p;
    t.base += counts * p;
    if (counts) advance(cpu, n, counts);
}

static void schedule_timer(int cpu, int n) {
    Timer& t = g_tm[ci(cpu)][n];
    int ev = (cpu == 9 ? EV_TIMER9 : EV_TIMER7) + n;
    if (!free_running(n, t)) { nds_cancel(ev); return; }
    nds_schedule(ev, t.base + (0x10000ull - t.counter) * period(t));
}

template <int CPU, int N> static void timer_event(uint64_t when) {
    update(CPU, N, when);
    schedule_timer(CPU, N);
}

uint16_t timer_read(int cpu, int n) {
    update(cpu, n, nds_now());
    return g_tm[ci(cpu)][n].counter;
}

void timer_write(int cpu, int n, int half, uint16_t v) {
    Timer& t = g_tm[ci(cpu)][n];
    uint64_t now = nds_now();
    update(cpu, n, now);
    io_set16(cpu, 0x04000100u + 4 * n + 2 * half, v);
    if (half == 0) { t.reload = v; return; }
    bool was = t.ctl & 0x80;
    t.ctl = v & 0xC7;
    if (!was && (t.ctl & 0x80)) { t.counter = t.reload; t.base = now; }
    else if (free_running(n, t)) t.base = std::max(t.base, now - (now - t.base) % period(t));
    schedule_timer(cpu, n);
}

// ---- DMA ------------------------------------------------------------------------------------
struct Dma { uint32_t src, dst, count; bool on; };
static Dma g_dma[2][4];

static uint32_t dma_mode(int cpu, uint16_t ctl) { return cpu == 9 ? (ctl >> 11) & 7 : (ctl >> 12) & 3; }

static void dma_run(int cpu, int ch) {
    Dma& d = g_dma[ci(cpu)][ch];
    uint32_t base = 0x040000B0u + 12 * ch;
    uint16_t ctl = io_reg16(cpu, base + 10);
    uint32_t unit = (ctl & 0x400) ? 4 : 2;
    int dctl = (ctl >> 5) & 3, sctl = (ctl >> 7) & 3;
    int32_t ds = dctl == 1 ? -(int32_t)unit : dctl == 2 ? 0 : (int32_t)unit;
    int32_t ss = sctl == 1 ? -(int32_t)unit : sctl == 2 ? 0 : (int32_t)unit;
    uint32_t s = d.src, t = d.dst;
    for (uint32_t i = 0; i < d.count; ++i) {
        if (unit == 4) {
            uint32_t v = cpu == 9 ? nds9::ld32(s & ~3u) : nds7::ld32(s & ~3u);
            cpu == 9 ? nds9::st32(t & ~3u, v) : nds7::st32(t & ~3u, v);
        } else {
            uint32_t v = cpu == 9 ? nds9::ld16(s & ~1u) : nds7::ld16(s & ~1u);
            cpu == 9 ? nds9::st16(t & ~1u, v) : nds7::st16(t & ~1u, v);
        }
        s += ss; t += ds;
    }
    d.src = s;
    d.dst = dctl == 3 ? io_reg(cpu, base + 4) : t;
    uint32_t mode = dma_mode(cpu, ctl);
    if ((ctl & 0x200) && mode != 0) {
        uint32_t n = io_reg16(cpu, base + 8) | (uint32_t)(io_reg16(cpu, base + 10) & 0x1F) << 16;
        d.count = n;                                // repeat: the count again (not the ARM7's top bits)
    } else {
        d.on = false;
        io_set16(cpu, base + 10, ctl & 0x7FFF);
    }
    if (ctl & 0x4000) nds_irq_raise(cpu, 8 + ch);
}

void dma_write(int cpu, int ch) {
    Dma& d = g_dma[ci(cpu)][ch];
    uint32_t base = 0x040000B0u + 12 * ch;
    uint16_t ctl = io_reg16(cpu, base + 10);
    if (!(ctl & 0x8000)) { d.on = false; return; }
    if (d.on) return;
    d.on = true;
    d.src = io_reg(cpu, base) & (cpu == 9 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    d.dst = io_reg(cpu, base + 4) & (cpu == 9 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    uint32_t n = io_reg16(cpu, base + 8);
    if (cpu == 9) n |= (uint32_t)(ctl & 0x1F) << 16;
    uint32_t maxn = cpu == 9 ? 0x200000u : ch == 3 ? 0x10000u : 0x4000u;
    n &= maxn - 1;
    d.count = n ? n : maxn;
    uint32_t mode = dma_mode(cpu, ctl);
    nds_trace("DMA%d.%d: %08X -> %08X, %u x %u, mode %u", cpu, ch, d.src, d.dst, d.count, (ctl & 0x400) ? 4 : 2, mode);
    if (mode == 0 || (cpu == 9 && mode == 7)) dma_run(cpu, ch);
    else if ((cpu == 9 && mode == 5) || (cpu == 7 && mode == 2)) { if (cart_word_ready()) dma_card(cpu); }
    else if (!(mode == 1 || (cpu == 9 && mode == 2)))
        nds_once("dma mode", (uint32_t)(cpu * 16 + mode), "ARM%d: DMA start mode %u not done", cpu, mode);
}

void dma_event(int cpu, int mode) {
    for (int ch = 0; ch < 4; ++ch) {
        Dma& d = g_dma[ci(cpu)][ch];
        if (d.on && dma_mode(cpu, io_reg16(cpu, 0x040000B0u + 12 * ch + 10)) == (uint32_t)mode) dma_run(cpu, ch);
    }
}

void dma_card(int cpu) {
    uint32_t mode = cpu == 9 ? 5 : 2;
    for (int ch = 0; ch < 4; ++ch) {
        Dma& d = g_dma[ci(cpu)][ch];
        if (d.on && dma_mode(cpu, io_reg16(cpu, 0x040000B0u + 12 * ch + 10)) == mode) {
            while (d.on && cart_word_ready()) dma_run(cpu, ch);
            return;
        }
    }
}

// ---- IPC ------------------------------------------------------------------------------------
static uint16_t g_sync[2];                          // each CPU's own bits: output nibble (8-11), IRQ enable (14)
static uint16_t g_fcnt[2];                          // each CPU's FIFO control: bits 2, 10, 14, 15
static std::deque<uint32_t> g_fifo[2];              // each CPU's send FIFO (the other's receive FIFO)
static uint32_t g_last[2];

static int other(int cpu) { return cpu == 9 ? 7 : 9; }

uint32_t ipc_read(int cpu, uint32_t addr, int size) {
    int me = ci(cpu), you = 1 - me;
    switch (addr & 0xFFFFFFu) {
    case 0x180: return (g_sync[you] >> 8 & 0xF) | (g_sync[me] & 0x4F00);
    case 0x184: {
        uint32_t v = g_fcnt[me] & 0xC404;
        if (g_fifo[me].empty()) v |= 1;
        if (g_fifo[me].size() >= 16) v |= 2;
        if (g_fifo[you].empty()) v |= 0x100;
        if (g_fifo[you].size() >= 16) v |= 0x200;
        return v;
    }
    case 0x100000: {
        (void)size;
        if (!(g_fcnt[me] & 0x8000)) return g_last[me];
        if (g_fifo[you].empty()) { g_fcnt[me] |= 0x4000; return g_last[me]; }
        uint32_t v = g_fifo[you].front();
        g_fifo[you].pop_front();
        g_last[me] = v;
        if (g_fifo[you].empty() && (g_fcnt[you] & 4)) nds_irq_raise(other(cpu), 17);
        return v;
    }
    }
    return 0;
}

void ipc_write(int cpu, uint32_t addr, uint32_t v, int size) {
    int me = ci(cpu), you = 1 - me;
    switch (addr & 0xFFFFFFu) {
    case 0x180:
        g_sync[me] = v & 0x4F00;
        if ((v & 0x2000) && (g_sync[you] & 0x4000)) nds_irq_raise(other(cpu), 16);
        return;
    case 0x184: {
        uint16_t old = g_fcnt[me];
        if (v & 8) g_fifo[me].clear();
        if (v & 0x4000) old &= ~0x4000;
        g_fcnt[me] = (old & 0x4000) | (v & 0x8404);
        if (!(old & 4) && (v & 4) && g_fifo[me].empty()) nds_irq_raise(cpu, 17);
        if (!(old & 0x400) && (v & 0x400) && !g_fifo[you].empty()) nds_irq_raise(cpu, 18);
        return;
    }
    case 0x188:
        if (size != 4) nds_once("ipc", size, "ARM%d: %d-byte write to IPCFIFOSEND", cpu, size);
        if (!(g_fcnt[me] & 0x8000)) return;
        if (g_fifo[me].size() >= 16) { g_fcnt[me] |= 0x4000; return; }
        g_fifo[me].push_back(v);
        if (g_fifo[me].size() == 1 && (g_fcnt[you] & 0x400)) nds_irq_raise(other(cpu), 18);
        return;
    }
}

// ---- the maths unit ---------------------------------------------------------------------------
static uint64_t rd64(uint32_t a) { return io_reg(9, a) | (uint64_t)io_reg(9, a + 4) << 32; }
static void wr64(uint32_t a, uint64_t v) { io_set(9, a, (uint32_t)v); io_set(9, a + 4, (uint32_t)(v >> 32)); }

static uint64_t isqrt(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

void math_write(uint32_t addr) {
    uint32_t off = addr - 0x04000000u;
    if (off >= 0x280 && off < 0x2B0) {
        uint32_t cnt = io_reg16(9, 0x04000280u);
        int mode = cnt & 3;
        int64_t num = (int64_t)rd64(0x04000290u), den = (int64_t)rd64(0x04000298u);
        int64_t q, r;
        if (mode == 0) {
            int32_t n = (int32_t)num, d = (int32_t)den;
            if (d == 0) { q = (int64_t)(uint32_t)(n < 0 ? 1 : -1) | (int64_t)(n < 0 ? 0xFFFFFFFF00000000ull : 0); r = n; }
            else if (n == INT32_MIN && d == -1) { q = 0x80000000ll; r = 0; }
            else { q = n / d; r = n % d; }
        } else {
            int64_t d = mode == 2 ? den : (int32_t)den;
            if (d == 0) { q = num < 0 ? 1 : -1; r = num; }
            else if (num == INT64_MIN && d == -1) { q = INT64_MIN; r = 0; }
            else { q = num / d; r = num % d; }
        }
        wr64(0x040002A0u, (uint64_t)q);
        wr64(0x040002A8u, (uint64_t)r);
        cnt = (cnt & 0x3) | (den == 0 ? 0x4000 : 0);
        io_set16(9, 0x04000280u, (uint16_t)cnt);
        return;
    }
    if (off >= 0x2B0 && off < 0x2C0) {
        uint32_t cnt = io_reg16(9, 0x040002B0u) & 1;
        uint64_t p = rd64(0x040002B8u);
        io_set(9, 0x040002B4u, (uint32_t)isqrt(cnt ? p : (uint32_t)p));
        io_set16(9, 0x040002B0u, (uint16_t)cnt);
    }
}

void system_reset() {
    std::memset(g_tm, 0, sizeof g_tm);
    std::memset(g_dma, 0, sizeof g_dma);
    nds_event_set(EV_TIMER9 + 0, timer_event<9, 0>);
    nds_event_set(EV_TIMER9 + 1, timer_event<9, 1>);
    nds_event_set(EV_TIMER9 + 2, timer_event<9, 2>);
    nds_event_set(EV_TIMER9 + 3, timer_event<9, 3>);
    nds_event_set(EV_TIMER7 + 0, timer_event<7, 0>);
    nds_event_set(EV_TIMER7 + 1, timer_event<7, 1>);
    nds_event_set(EV_TIMER7 + 2, timer_event<7, 2>);
    nds_event_set(EV_TIMER7 + 3, timer_event<7, 3>);
}
