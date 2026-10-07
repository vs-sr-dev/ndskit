// ndskit runtime — no hardware: the services for builds that only run the
// game's own code (the self-test, the link check). A hardware access, a SWI,
// an undefined instruction or a call to a non-entry is reported and ends the
// run; so does a return that went elsewhere, which would mean the
// recompiled control flow is wrong. CP15 keeps what is written to it and
// gives it back, as armemu does with no handler set.
#include "arm.h"
#include <cstdio>
#include <cstdlib>

static void fail(ArmContext& c, const char* what, uint32_t a) {
    std::fprintf(stderr, "ndskit: %s %08X (ARM%d, lr %08X, sp %08X)\n", what, a, c.cpu, c.r[14], c.r[13]);
    std::exit(1);
}

// Without hardware nothing can end a wait: a call that spends its whole
// budget of safe points (backward jumps, calls) is stuck.
void arm_poll(ArmContext& c) { fail(c, "safe-point budget spent; lr", c.r[14]); }

void arm_swi(ArmContext& c, uint32_t num, uint32_t ret) { (void)ret; fail(c, "swi (no BIOS)", num); }
void arm_undefined(ArmContext& c, uint32_t pc) { fail(c, "undefined instruction at", pc); }
void arm_bad_return(ArmContext& c, uint32_t expected) {
    std::fprintf(stderr, "ndskit: returned to %08X, expected %08X\n", c.pc, expected);
    std::exit(1);
}
void arm_call_unknown(ArmContext& c, uint32_t key) { fail(c, "call to a non-entry", key); }

static uint32_t g_cp15[8][16][16][8];

uint32_t arm_cp15_read(ArmContext& c, int op1, int crn, int crm, int op2) {
    (void)c;
    return g_cp15[op1 & 7][crn & 15][crm & 15][op2 & 7];
}

void arm_cp15_write(ArmContext& c, int op1, int crn, int crm, int op2, uint32_t v) {
    (void)c;
    g_cp15[op1 & 7][crn & 15][crm & 15][op2 & 7] = v;
}

uint32_t nds_mmio_read(int cpu, uint32_t a, int size) {
    std::fprintf(stderr, "ndskit: ARM%d %d-byte read of %08X (no hardware)\n", cpu, size, a);
    std::exit(1);
}
void nds_mmio_write(int cpu, uint32_t a, uint32_t v, int size) {
    std::fprintf(stderr, "ndskit: ARM%d %d-byte write of %08X to %08X (no hardware)\n", cpu, size, v, a);
    std::exit(1);
}
