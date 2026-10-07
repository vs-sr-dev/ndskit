// ndskit runtime — coroutines: host stacks that hand control to one another
// explicitly (Windows fibers; ucontext elsewhere). The machine runs each CPU,
// and each of a CPU's guest threads, on its own: recompiled code nests its
// calls on the host stack, so a guest thread that is switched out keeps its
// host frames where they are until it is switched back in.
#pragma once
#include <cstddef>

struct Coro;
typedef void (*CoroFunc)(void* arg);

Coro* coro_main();                                      // the calling thread, as a coroutine
Coro* coro_new(CoroFunc f, void* arg, size_t stack);    // starts on its first coro_switch
void  coro_switch(Coro* to);                            // from the running coroutine
Coro* coro_self();
