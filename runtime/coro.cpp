// ndskit runtime — coroutines (coro.h).
#include "coro.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>

struct Coro { void* fiber; CoroFunc f; void* arg; };
static Coro* g_self;

static void WINAPI start(void* p) {
    Coro* c = (Coro*)p;
    c->f(c->arg);
    std::fprintf(stderr, "ndskit: a coroutine returned\n");
    std::exit(1);
}

Coro* coro_main() {
    static Coro m;
    if (!m.fiber) {
        m.fiber = ConvertThreadToFiber(nullptr);
        if (!m.fiber) m.fiber = GetCurrentFiber();
        g_self = &m;
    }
    return &m;
}

Coro* coro_new(CoroFunc f, void* arg, size_t stack) {
    Coro* c = new Coro{nullptr, f, arg};
    c->fiber = CreateFiberEx(64 * 1024, stack, 0, start, c);
    if (!c->fiber) { std::fprintf(stderr, "ndskit: CreateFiberEx failed\n"); std::exit(1); }
    return c;
}

void coro_switch(Coro* to) {
    g_self = to;
    SwitchToFiber(to->fiber);
}

#else
#include <ucontext.h>

struct Coro { ucontext_t uc; CoroFunc f; void* arg; };
static Coro* g_self;

static void start(unsigned lo, unsigned hi) {
    Coro* c = (Coro*)((uintptr_t)lo | (uintptr_t)hi << 32);
    c->f(c->arg);
    std::fprintf(stderr, "ndskit: a coroutine returned\n");
    std::exit(1);
}

Coro* coro_main() {
    static Coro m;
    if (!g_self) g_self = &m;
    return &m;
}

Coro* coro_new(CoroFunc f, void* arg, size_t stack) {
    Coro* c = new Coro{};
    c->f = f; c->arg = arg;
    getcontext(&c->uc);
    c->uc.uc_stack.ss_sp = std::malloc(stack);
    c->uc.uc_stack.ss_size = stack;
    c->uc.uc_link = nullptr;
    uintptr_t p = (uintptr_t)c;
    makecontext(&c->uc, (void (*)())start, 2, (unsigned)p, (unsigned)(p >> 32));
    return c;
}

void coro_switch(Coro* to) {
    Coro* from = g_self;
    g_self = to;
    swapcontext(&from->uc, &to->uc);
}
#endif

Coro* coro_self() { return g_self; }
