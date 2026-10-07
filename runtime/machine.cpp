// ndskit runtime — the machine: the two CPUs in time, events, interrupts,
// and the guest's threads.
//
// Time. The recompiled code has no cycle counts; it has safe points (loop
// back-edges, calls, msr). A safe point stands for a fixed number of ticks
// (g_cfg.ticks9, ticks7). The machine runs in quanta (a quarter of a line,
// or up to the next event): the ARM9 runs until its clock passes the
// quantum's end, then the ARM7, then the events due are fired (lines,
// timers, the card) and raise interrupts. A run is the same run every time.
// A CPU that halts (CP15's wait for interrupt, HALTCNT, a BIOS wait) skips
// its quanta until IE & IF.
//
// Threads. Recompiled code nests its calls on the host stack, so each
// guest thread runs on its own coroutine (coro.h), and a CPU runs one at a
// time. A thread is switched out where the guest switches contexts:
//   * an exception return in recompiled code (the SDK's OS_LoadContext:
//     `ldm r0, {r0-r14}^; subs pc, lr, #4`), whose target is where the loaded
//     context goes;
//   * the BIOS's return from an interrupt (bios_irq), when the SDK's handler
//     has put another thread's context in the IRQ stack's frame.
// The target names the thread: an interrupt taken at a safe point pushes a
// marker as the return address (0x0F000000 + 16 x the thread's number),
// which the SDK saves in the thread's context and gives back when it loads
// it; a context saved by OS_SaveContext (a function that returns twice,
// recomp.discover) is found by its sp, which the runtime noted when the save
// returned (arm_context_saved); a function's entry is a new thread. A thread
// switched back in at its save point returns there through `bx lr`, and the
// recompiled caller of the save unwinds to it (ArmUnwind).
#include "nds.h"
#include "coro.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>

NdsConfig g_cfg;

static NdsCpu g_cpus[2];
static int g_running;
static uint64_t g_now, g_qend, g_vblanks;
static const char* g_stop;
static const uint64_t kQuantum = kLineTicks / 4;
static const int32_t kBudget = 32;
static int32_t g_budget_start[2];

NdsCpu& nds_cpu(int cpu) { return g_cpus[cpu == 9 ? 0 : 1]; }
int nds_running() { return g_running; }
uint64_t nds_now() { return g_running ? nds_cpu(g_running).time : g_now; }
uint64_t nds_vblanks() { return g_vblanks; }
void nds_count_vblank() { input_vblank(++g_vblanks); }

// ---- logging -----------------------------------------------------------------------------
static void vlog(const char* tag, const char* fmt, va_list ap) {
    uint64_t t = nds_now();
    std::fprintf(stderr, "[%5llu:%03llu %s] ", (unsigned long long)(t / kFrameTicks),
                 (unsigned long long)(t % kFrameTicks / kLineTicks), tag);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

static const char* tag() { return g_running == 9 ? "9" : g_running == 7 ? "7" : "-"; }

void nds_note(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(tag(), fmt, ap); va_end(ap);
}

void nds_trace(const char* fmt, ...) {
    if (!g_cfg.trace) return;
    va_list ap; va_start(ap, fmt); vlog(tag(), fmt, ap); va_end(ap);
}

void nds_once(const char* what, uint32_t key, const char* fmt, ...) {
    static std::set<std::pair<std::string, uint32_t>> seen;
    if (!seen.insert({what, key}).second) return;
    va_list ap; va_start(ap, fmt); vlog(tag(), fmt, ap); va_end(ap);
}

static void report();

void nds_fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(tag(), fmt, ap); va_end(ap);
    report();
    std::fflush(stderr);
    std::_Exit(1);
}

// ---- events ------------------------------------------------------------------------------
struct Event { uint64_t when; NdsEventFn fn; bool on; };
static Event g_ev[EV_COUNT];

void nds_event_set(int ev, NdsEventFn fn) { g_ev[ev].fn = fn; }
void nds_schedule(int ev, uint64_t when) { g_ev[ev].when = when; g_ev[ev].on = true; }
void nds_cancel(int ev) { g_ev[ev].on = false; }

static uint64_t next_event() {
    uint64_t t = UINT64_MAX;
    for (auto& e : g_ev) if (e.on && e.when < t) t = e.when;
    return t;
}

static void fire_events() {
    for (;;) {
        int best = -1;
        for (int i = 0; i < EV_COUNT; ++i)
            if (g_ev[i].on && g_ev[i].when <= g_now && (best < 0 || g_ev[i].when < g_ev[best].when)) best = i;
        if (best < 0) return;
        g_ev[best].on = false;
        g_ev[best].fn(g_ev[best].when);
    }
}

// ---- threads -------------------------------------------------------------------------------
struct Thread {
    Coro* co;
    int cpu, num;
    uint32_t marker;                    // the return address an interrupt taken in it pushes
    uint32_t key;                       // where it was last switched in (its entry, first)
    bool saved;                         // a context save returned in it and it has not run on since
    uint32_t saved_sp;
};
static std::vector<Thread*> g_threads[2];
static Thread* g_cur[2];
static Coro* g_main_co;
static const size_t kStack = 16u << 20;

static int ci(int cpu) { return cpu == 9 ? 0 : 1; }
static bool is_marker(uint32_t a) { return (a & 0xFFF0000Fu) == 0x0F000000u; }

static void thread_main(void* p) {
    Thread* t = (Thread*)p;
    ArmContext& c = nds_cpu(t->cpu).c;
    uint32_t key = t->key;
    for (;;) {
        try {
            arm_call(c, key);
        } catch (const ArmUnwind& u) {
            nds_fatal("thread %d: an unwind to %08X reached its base", t->num, u.pc);
        }
        if (!arm_lookup(c.cpu, c.pc)) nds_fatal("thread %d (from %08X) returned to %08X", t->num, t->key, c.pc);
        key = c.pc;                     // returned to an entry: the SDK's OS_ExitThread
    }
}

static Thread* new_thread(int cpu, uint32_t key) {
    Thread* t = new Thread{};
    t->cpu = cpu;
    t->num = (int)g_threads[ci(cpu)].size();
    t->marker = 0x0F000000u + 16u * (uint32_t)t->num;
    t->key = key;
    t->co = coro_new(thread_main, t, kStack);
    g_threads[ci(cpu)].push_back(t);
    nds_trace("ARM%d: thread %d starts at %08X", cpu, t->num, key);
    return t;
}

// The thread a context goes to, from its target and its sp.
static Thread* thread_for(ArmContext& c, uint32_t key) {
    auto& ts = g_threads[ci(c.cpu)];
    if (is_marker(key)) {
        uint32_t n = (key - 0x0F000000u) / 16;
        if (n < ts.size()) return ts[n];
        nds_fatal("a context goes to the marker %08X of no thread", key);
    }
    for (Thread* t : ts)
        if (t->saved && t->saved_sp == c.r[13]) return t;
    if (arm_lookup(c.cpu, key)) return new_thread(c.cpu, key);
    nds_fatal("a context goes to %08X (sp %08X): no thread saved there, not an entry", key, c.r[13]);
}

// Switch the running CPU to thread t, which goes to `key`; returns when the
// calling thread is switched back in (its own key is then t->key).
static void switch_to(int cpu, Thread* t, uint32_t key) {
    t->key = key;
    t->saved = false;
    g_cur[ci(cpu)] = t;
    nds_trace("ARM%d: to thread %d at %08X", cpu, t->num, key);
    coro_switch(t->co);
}

void arm_context_saved(ArmContext& c) {
    Thread* t = g_cur[ci(c.cpu)];
    t->saved = true;
    t->saved_sp = c.r[13];
}

static uint32_t ld_insn(int cpu, uint32_t a) {
    uint32_t v = 0;
    nds_mem_read(cpu, a & ~1u, &v, (a & 1) ? 2 : 4);
    return v;
}

// An exception return in recompiled code: CPSR = SPSR, then to the target.
void arm_exception_return(ArmContext& c, uint32_t target) {
    arm_restore_cpsr(c, target);
    Thread* self = g_cur[ci(c.cpu)];
    Thread* t = thread_for(c, c.pc);
    if (t == self) nds_fatal("an exception return to %08X in its own thread", c.pc);
    switch_to(c.cpu, t, c.pc);
    // switched back in: at the return of a context save (`bx lr` there)
    uint32_t k = self->key;
    uint32_t ins = ld_insn(c.cpu, k);
    if ((k & 1) ? (ins & 0xFFFF) == 0x4770u : ins == 0xE12FFF1Eu) {
        c.pc = arm_iw(c.r[14]);
        return;
    }
    nds_fatal("thread %d switched back in at %08X, not a return", self->num, k);
}

// The BIOS's return from an interrupt (bios_irq): the frame popped, CPSR =
// SPSR_irq, c.pc the target. Back to the interrupted code, or to another thread.
void nds_irq_return(ArmContext& c) {
    Thread* self = g_cur[ci(c.cpu)];
    if (c.pc == self->marker) return;
    Thread* t = thread_for(c, c.pc);
    if (t == self) nds_fatal("the interrupt returned to %08X in its own thread", c.pc);
    switch_to(c.cpu, t, c.pc);
    if (self->key != self->marker)
        nds_fatal("thread %d (interrupted) switched back in at %08X", self->num, self->key);
}

uint32_t nds_irq_marker(int cpu) {
    Thread* t = g_cur[ci(cpu)];
    t->saved = false;
    return t->marker;
}

// ---- interrupts and time on a CPU ----------------------------------------------------------
static void force_poll(NdsCpu& cpu) {
    int i = ci(cpu.id);
    if (cpu.c.budget > 0) { g_budget_start[i] -= cpu.c.budget; cpu.c.budget = 0; }
}

void nds_irq_update(int cpu) {
    NdsCpu& p = nds_cpu(cpu);
    p.c.irq = (p.ime & 1) && (p.ie & p.iflags);
    if (p.c.irq && !p.c.i) force_poll(p);
}

static uint64_t g_irq_count[2][32];

void nds_irq_raise(int cpu, int bit) {
    NdsCpu& p = nds_cpu(cpu);
    p.iflags |= 1u << bit;
    ++g_irq_count[ci(cpu)][bit];
    nds_irq_update(cpu);
}

static void yield() { coro_switch(g_main_co); }

static void take_irqs(ArmContext& c) {
    for (int n = 0; c.irq && !c.i && n < 8; ++n) bios_irq(c);
}

static uint32_t guest_of(int cpu, const void* p);

// The calls traced (--calls): the last entries of each CPU, with lr and the thread.
int g_arm_trace;
struct Call { uint32_t key, lr; int thread; uint64_t vbl; };
static const unsigned kCalls = 1u << 16;
static Call g_calls[2][kCalls];
static unsigned g_calls_pos[2];

void arm_trace_enter(ArmContext& c, uint32_t key) {
    int i = ci(c.cpu);
    Thread* t = g_cur[i];
    g_calls[i][g_calls_pos[i]++ % kCalls] = {key, c.r[14], t ? t->num : -1, g_vblanks};
    if (key == g_cfg.break_at && c.cpu == g_cfg.break_cpu) nds_fatal("break at %08X (lr %08X)", key, c.r[14]);
}

// The last n calls, a run of the same call and caller printed once with its count.
static void print_calls(int cpu, unsigned n) {
    int i = ci(cpu);
    unsigned end = g_calls_pos[i], start = end > kCalls ? end - kCalls : 0;
    std::vector<std::pair<Call, unsigned>> runs;
    for (unsigned k = start; k < end; ++k) {
        const Call& e = g_calls[i][k % kCalls];
        if (!runs.empty() && runs.back().first.key == e.key && runs.back().first.lr == e.lr) ++runs.back().second;
        else runs.push_back({e, 1});
    }
    std::fprintf(stderr, "    last calls:\n");
    for (size_t k = runs.size() > n ? runs.size() - n : 0; k < runs.size(); ++k) {
        const Call& e = runs[k].first;
        std::fprintf(stderr, "      [v%llu t%d] %08X  lr %08X", (unsigned long long)e.vbl, e.thread, e.key, e.lr);
        if (runs[k].second > 1) std::fprintf(stderr, "  x%u", runs[k].second);
        std::fprintf(stderr, "\n");
    }
}

// A store into the watched range: the value, and where from (the recompiled
// function that stored, by its host return address; the thread; lr).
void nds_watch_hit(int cpu, uint32_t a, uint32_t v, int size) {
    uint32_t fn = guest_of(cpu, __builtin_return_address(0));
    const ArmContext& c = nds_cpu(cpu).c;
    Thread* t = g_cur[ci(cpu)];
    nds_note("watch: ARM%d %d-byte store of %08X to %08X in %08X (thread %d, lr %08X, sp %08X)", cpu, size, v, a, fn,
             t ? t->num : -1, c.r[14], c.r[13]);
}

// Where each CPU has been polling: the host return address of its last
// polls lies in the recompiled function that polled; the modules' tables map
// it back to that function's guest entry (the nearest entry below it in the
// host's code, as the compiler lays out a file's functions in order).
static void* g_ring[2][256];
static unsigned g_ring_pos[2];

struct HostFn { uintptr_t host; uint32_t key; };
static std::vector<HostFn> g_hostfns[2];

static uint32_t guest_of(int cpu, const void* p) {
    auto& v = g_hostfns[ci(cpu)];
    if (v.empty()) {
        for (int m = 0; m < g_arm_nmodules; ++m)
            if (g_arm_modules[m]->cpu == cpu)
                for (uint32_t k = 0; k < g_arm_modules[m]->nfuncs; ++k)
                    v.push_back({(uintptr_t)g_arm_modules[m]->funcs[k].fn, g_arm_modules[m]->funcs[k].key});
        std::sort(v.begin(), v.end(), [](const HostFn& a, const HostFn& b) { return a.host < b.host; });
    }
    auto it = std::upper_bound(v.begin(), v.end(), (uintptr_t)p,
                               [](uintptr_t x, const HostFn& f) { return x < f.host; });
    return it == v.begin() ? 0 : (it - 1)->key;
}

static void hot_spots(int cpu) {
    std::vector<std::pair<uint32_t, int>> n;
    for (void* p : g_ring[ci(cpu)]) {
        if (!p) continue;
        uint32_t k = guest_of(cpu, p);
        auto it = std::find_if(n.begin(), n.end(), [&](auto& e) { return e.first == k; });
        if (it == n.end()) n.push_back({k, 1}); else ++it->second;
    }
    std::sort(n.begin(), n.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "    polling in:");
    for (size_t i = 0; i < n.size() && i < 8; ++i) std::fprintf(stderr, " %08X (%d)", n[i].first, n[i].second);
    std::fprintf(stderr, "\n");
}

void arm_poll(ArmContext& c) {
    NdsCpu& cpu = nds_cpu(c.cpu);
    int i = ci(c.cpu);
    g_ring[i][g_ring_pos[i]++ % 256] = __builtin_return_address(0);
    cpu.time += (uint64_t)(g_budget_start[i] - c.budget) * (c.cpu == 9 ? g_cfg.ticks9 : g_cfg.ticks7);
    c.budget = g_budget_start[i] = kBudget;
    if (cpu.time >= g_qend) yield();
    if (c.irq && !c.i) take_irqs(c);
}

void nds_halt(ArmContext& c) {
    NdsCpu& cpu = nds_cpu(c.cpu);
    cpu.time += (uint64_t)(g_budget_start[ci(c.cpu)] - c.budget) * (c.cpu == 9 ? g_cfg.ticks9 : g_cfg.ticks7);
    c.budget = g_budget_start[ci(c.cpu)] = kBudget;
    cpu.halted = true;
    while (cpu.halted) yield();
    if (c.irq && !c.i) take_irqs(c);
}

// Time a BIOS call takes: past the quantum's end, the other CPU runs first
// (a wait by loop is how one CPU gives the other time to answer).
void nds_spend(ArmContext& c, uint64_t ticks) {
    NdsCpu& cpu = nds_cpu(c.cpu);
    cpu.time += ticks;
    if (cpu.time >= g_qend) yield();
}

void nds_stop(const char* why) { if (!g_stop) g_stop = why; }

// ---- services the recompiled code calls ---------------------------------------------------
void arm_swi(ArmContext& c, uint32_t num, uint32_t ret) {
    (void)ret;
    bios_swi(c, num);
}

void arm_undefined(ArmContext& c, uint32_t pc) { nds_fatal("undefined instruction at %08X (ARM%d)", pc, c.cpu); }

void arm_bad_return(ArmContext& c, uint32_t expected) {
    if (arm_is_landing(c.cpu, c.pc)) throw ArmUnwind{c.pc};
    nds_fatal("ARM%d returned to %08X, expected %08X", c.cpu, c.pc, expected);
}

// A call to code the modules do not have. If it is code the program copied
// there from its own image, say where from: the recompiler takes it with --copy.
void arm_call_unknown(ArmContext& c, uint32_t key) {
    uint8_t here[32];
    uint32_t a = key & ~1u;
    if (nds_mem_read(c.cpu, a, here, sizeof here)) {
        for (int m = 0; m < g_arm_nmodules; ++m) {
            const ArmModule* mod = g_arm_modules[m];
            if (mod->cpu != c.cpu) continue;
            for (uint32_t s = 0; s < mod->nsegs; ++s) {
                const ArmSegment& g = mod->segs[s];
                if (g.copied || (a - g.base) < g.size) continue;
                std::vector<uint8_t> img(g.size);
                if (!nds_mem_read(c.cpu, g.base, img.data(), g.size)) continue;
                for (uint32_t o = 0; o + sizeof here <= g.size; o += 2)
                    if (!std::memcmp(&img[o], here, sizeof here)) {
                        // how far the copy runs
                        uint32_t n = 0;
                        uint8_t b;
                        while (o + n < g.size && nds_mem_read(c.cpu, a + n, &b, 1) && b == img[o + n]) ++n;
                        nds_note("the code at %08X is the program's %08X, copied there (at least %X bytes): "
                                 "recompile with --copy %d:%08X=%08X:%X", a, g.base + o, n, c.cpu, a, g.base + o, n);
                        s = mod->nsegs;
                        m = g_arm_nmodules;
                        break;
                    }
            }
        }
    }
    nds_fatal("ARM%d: call to %08X, not an entry (lr %08X)", c.cpu, key, c.r[14]);
}

// CP15 (the ARM9): the TCMs' places, the waits for an interrupt; the rest kept.
static uint32_t g_cp15[8][16][16][8];
static uint32_t g_cp15_control = 0x00052078u;

static void tcm_update() {
    uint32_t r = g_cp15[0][9][1][0];
    g_dtcm_base = r & 0xFFFFF000u;
    g_dtcm_size = (g_cp15_control >> 16 & 1) ? 0x4000u : 0;
}

uint32_t arm_cp15_read(ArmContext& c, int op1, int crn, int crm, int op2) {
    if (c.cpu != 9) arm_undefined(c, 0);
    if (op1 == 0 && crn == 0 && crm == 0) return op2 == 1 ? 0x0F0D2112u : op2 == 2 ? 0x00140180u : 0x41059461u;
    if (op1 == 0 && crn == 1 && crm == 0 && op2 == 0) return g_cp15_control;
    return g_cp15[op1 & 7][crn & 15][crm & 15][op2 & 7];
}

void arm_cp15_write(ArmContext& c, int op1, int crn, int crm, int op2, uint32_t v) {
    if (c.cpu != 9) arm_undefined(c, 0);
    if (op1 == 0 && crn == 1 && crm == 0 && op2 == 0) {
        g_cp15_control = (v & 0x000FF085u) | 0x78u;
        tcm_update();
        return;
    }
    g_cp15[op1 & 7][crn & 15][crm & 15][op2 & 7] = v;
    if (crn == 9 && crm == 1) tcm_update();
    if ((crn == 7 && crm == 0 && op2 == 4) || (crn == 7 && crm == 8 && op2 == 2)) nds_halt(c);
}

void nds_cp15_boot() {
    g_cp15[0][9][1][0] = 0x0300000Au;   // DTCM at 0x03000000, 16 KiB (melonDS's direct boot)
    g_cp15[0][9][1][1] = 0x00000020u;   // ITCM: 32 MiB at 0
    tcm_update();
}

// ---- the run -------------------------------------------------------------------------------
static void run_cpu(int cpu) {
    NdsCpu& p = nds_cpu(cpu);
    if (p.time >= g_qend) return;
    if (p.halted) {
        if (!(p.ie & p.iflags)) { p.time = g_qend; return; }
        p.halted = false;
        if (p.time < g_now) p.time = g_now;
    }
    g_running = cpu;
    coro_switch(g_cur[ci(cpu)]->co);
    g_running = 0;
}

static const char* mode_name(uint32_t m) {
    switch (m) {
    case 0x10: return "usr"; case 0x11: return "fiq"; case 0x12: return "irq"; case 0x13: return "svc";
    case 0x17: return "abt"; case 0x1B: return "und"; case 0x1F: return "sys"; default: return "?";
    }
}

static void report() {
    static bool once;
    if (once) return;
    once = true;
    std::fprintf(stderr, "  %llu VBlanks, %.3f s of DS time\n", (unsigned long long)g_vblanks,
                 nds_now() / 67027964.0);
    for (int cpu : {9, 7}) {
        NdsCpu& p = nds_cpu(cpu);
        const ArmContext& c = p.c;
        std::fprintf(stderr, "  ARM%d %s%s, thread %d of %zu, sp %08X lr %08X, IME %u IE %08X IF %08X\n", cpu,
                     mode_name(c.mode), p.halted ? " halted" : "", g_cur[ci(cpu)] ? g_cur[ci(cpu)]->num : -1,
                     g_threads[ci(cpu)].size(), c.r[13], c.r[14], p.ime, p.ie, p.iflags);
        std::fprintf(stderr, "    r0-r12:");
        for (int i = 0; i < 13; ++i) std::fprintf(stderr, " %08X", c.r[i]);
        std::fprintf(stderr, "\n    interrupts:");
        for (int b = 0; b < 32; ++b)
            if (g_irq_count[ci(cpu)][b]) std::fprintf(stderr, " %d:%llu", b, (unsigned long long)g_irq_count[ci(cpu)][b]);
        std::fprintf(stderr, "\n");
        hot_spots(cpu);
        if (g_arm_trace) print_calls(cpu, (unsigned)g_cfg.calls);
        // the words on its stack that point into a module's segments: return addresses, mostly
        std::fprintf(stderr, "    stack:");
        int n = 0;
        for (uint32_t a = c.r[13] & ~3u; a < (c.r[13] & ~3u) + 0x400 && n < 20; a += 4) {
            uint32_t v;
            if (!nds_mem_read(cpu, a, &v, 4)) break;
            for (int m = 0; m < g_arm_nmodules; ++m) {
                const ArmModule* mod = g_arm_modules[m];
                if (mod->cpu != cpu) continue;
                for (uint32_t s = 0; s < mod->nsegs; ++s)
                    if (v - mod->segs[s].base < mod->segs[s].size && !arm_lookup(cpu, v)) {
                        std::fprintf(stderr, " %08X", v);
                        ++n;
                        s = mod->nsegs;
                        m = g_arm_nmodules;
                    }
            }
        }
        std::fprintf(stderr, "\n");
    }
}

static void write_file(const std::string& path, const void* p, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { nds_note("cannot write %s", path.c_str()); return; }
    std::fwrite(p, 1, n, f);
    std::fclose(f);
}

static void dump() {
    write_file(g_cfg.out + "/main.bin", g_main, 0x400000);
    write_file(g_cfg.out + "/itcm.bin", g_itcm, 0x8000);
    write_file(g_cfg.out + "/dtcm.bin", g_dtcm, 0x4000);
    write_file(g_cfg.out + "/wram7.bin", g_wram7, 0x10000);
    write_file(g_cfg.out + "/swram.bin", g_swram, 0x8000);
    FILE* f = std::fopen((g_cfg.out + "/cpus.txt").c_str(), "w");
    if (f) {
        for (int cpu : {9, 7}) {
            const ArmContext& c = nds_cpu(cpu).c;
            std::fprintf(f, "ARM%d cpsr %08X", cpu, arm_get_cpsr(c));
            for (int i = 0; i < 15; ++i) std::fprintf(f, " r%d %08X", i, c.r[i]);
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "dtcm %08X\nvblanks %llu\n", g_dtcm_base, (unsigned long long)g_vblanks);
        std::fclose(f);
    }
}

static const ArmModule* find_module(int cpu) {
    for (int i = 0; i < g_arm_nmodules; ++i) {
        const ArmModule* m = g_arm_modules[i];
        if (m->cpu != cpu || !m->nsegs) continue;
        const ArmSegment& s = m->segs[0];          // the block at its load address: the autoloads are not in place yet
        std::vector<uint8_t> buf(s.size);
        if (nds_mem_read(cpu, s.base, buf.data(), s.size) && nds_crc32(buf.data(), s.size) == s.crc) return m;
    }
    return nullptr;
}

int nds_main(const NdsConfig& cfg) {
    g_cfg = cfg;
    g_arm_trace = cfg.calls > 0 || cfg.break_at;
    g_watch_lo = cfg.watch_lo;
    g_watch_len = cfg.watch_len;
    if (!cart_open(cfg.rom)) { std::fprintf(stderr, "cannot open %s\n", cfg.rom.c_str()); return 2; }
    for (int cpu : {9, 7}) {
        NdsCpu& p = nds_cpu(cpu);
        std::memset(&p, 0, sizeof p);
        p.id = cpu;
        p.c.cpu = cpu;
        p.c.mode = 0x1F;
        p.c.budget = g_budget_start[ci(cpu)] = kBudget;
    }
    mmio_reset();
    input_parse(cfg.input);
    system_reset();
    video_reset();
    spi_reset();
    nds_cp15_boot();
    bios_direct_boot();
    const std::vector<uint8_t>& rom = cart_rom();
    uint32_t entry9, entry7;
    std::memcpy(&entry9, &rom[0x24], 4);
    std::memcpy(&entry7, &rom[0x34], 4);
    g_main_co = coro_main();
    for (int cpu : {9, 7}) {
        const ArmModule* m = find_module(cpu);
        if (!m) { std::fprintf(stderr, "no module of this build matches the ARM%d program\n", cpu); return 2; }
        arm_activate(m);
        nds_note("ARM%d: module %s, entry %08X", cpu, m->name, cpu == 9 ? entry9 : entry7);
        g_cur[ci(cpu)] = new_thread(cpu, cpu == 9 ? entry9 : entry7);
    }
    while (!g_stop) {
        g_qend = std::min(next_event(), g_now + kQuantum);
        run_cpu(9);
        run_cpu(7);
        g_now = g_qend;
        fire_events();
        if (cfg.stop_vblanks && g_vblanks >= cfg.stop_vblanks) nds_stop("VBlank limit");
    }
    nds_note("stopped: %s", g_stop);
    report();
    if (cfg.dump) dump();
    cart_save();
    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(0);                      // the threads' coroutines are parked: do not unwind them
}
