// ndskit runtime — the ARM7's sound registers (the mixer is phase 6).
//
// The 16 channels and the capture units are kept as written; a channel
// started in one-shot mode reads busy until its samples would have played
// (the sequencer watches the bit), PSG, noise and looped channels until
// they are stopped.
#include "nds.h"
#include <cstring>

static uint8_t g_regs[0x120];                       // 0x04000400-0x0400051F
static uint64_t g_end[16];                          // when a one-shot channel stops
static struct Bias { Bias() { g_regs[0x104] = 0x00; g_regs[0x105] = 0x02; } } g_bias;   // SOUNDBIAS 0x200

static uint32_t reg32(uint32_t off) { uint32_t v; std::memcpy(&v, &g_regs[off], 4); return v; }

static void started(int ch) {
    uint32_t base = 0x10u * ch, cnt = reg32(base);
    int fmt = (cnt >> 29) & 3, rep = (cnt >> 27) & 3;
    if (rep != 2 || fmt == 3) { g_end[ch] = UINT64_MAX; return; }
    uint16_t tmr, pnt;
    std::memcpy(&tmr, &g_regs[base + 8], 2);
    std::memcpy(&pnt, &g_regs[base + 0xA], 2);
    uint64_t bytes = 4ull * (pnt + (reg32(base + 0xC) & 0x3FFFFF));
    uint64_t samples = fmt == 0 ? bytes : fmt == 1 ? bytes / 2 : bytes > 4 ? (bytes - 4) * 2 : 0;
    g_end[ch] = nds_now() + samples * (0x10000u - tmr) * 4;
}

void sound_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = addr - 0x04000400u;
    if (off + size > sizeof g_regs) return;
    bool was[16];
    for (int ch = 0; ch < 16; ++ch) was[ch] = g_regs[0x10 * ch + 3] & 0x80;
    std::memcpy(&g_regs[off], &v, size);
    if (off < 0x100) {
        int ch = (int)(off >> 4);
        if (!was[ch] && (g_regs[0x10 * ch + 3] & 0x80)) started(ch);
    }
    io_set16(7, addr & ~1u, (uint16_t)(g_regs[off & ~1u] | g_regs[(off & ~1u) + 1] << 8));
}

uint32_t sound_read(uint32_t addr, int size) {
    uint32_t off = addr - 0x04000400u;
    if (off + size > sizeof g_regs) return 0;
    if (off < 0x100) {
        int ch = (int)(off >> 4);
        if ((g_regs[0x10 * ch + 3] & 0x80) && nds_now() >= g_end[ch]) g_regs[0x10 * ch + 3] &= 0x7F;
    }
    uint32_t v = 0;
    std::memcpy(&v, &g_regs[off], size);
    return v;
}
