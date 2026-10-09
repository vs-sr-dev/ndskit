// ndskit runtime — the address map outside the host arrays of arm.h: the
// I/O registers of each CPU, the palettes, VRAM through its banks, OAM, the
// GBA slot (empty), the BIOS areas, the ARM7's wireless block.
//
// Each CPU's registers 0x04000000-0x04001FFF are kept as plain storage; the
// ones with an effect or a live value go to their device. Accesses are cut
// into halfwords (a word low half first) except the registers that are
// words to the hardware: the IPC and card FIFOs, the geometry engine's
// ports, the maths unit.
#include "nds.h"
#include <cstring>

static uint8_t g_io[2][0x2000];
static uint8_t g_wifi[0x10000];
uint8_t g_palette[0x800], g_oam[0x800];
static uint16_t g_keys = 0x03FF, g_extkeys = 0x007F;

static int ci(int cpu) { return cpu == 9 ? 0 : 1; }

uint16_t io_reg16(int cpu, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, &g_io[ci(cpu)][(a - 0x04000000u) & 0x1FFE], 2);
    return v;
}
void io_set16(int cpu, uint32_t a, uint16_t v) { std::memcpy(&g_io[ci(cpu)][(a - 0x04000000u) & 0x1FFE], &v, 2); }
uint32_t io_reg(int cpu, uint32_t a) { return io_reg16(cpu, a) | (uint32_t)io_reg16(cpu, a + 2) << 16; }
void io_set(int cpu, uint32_t a, uint32_t v) { io_set16(cpu, a, (uint16_t)v); io_set16(cpu, a + 2, (uint16_t)(v >> 16)); }

void mmio_set_keys(uint16_t keys, uint16_t ext) { g_keys = keys; g_extkeys = ext; }

void mmio_reset() {
    std::memset(g_io, 0, sizeof g_io);
    std::memset(g_wifi, 0, sizeof g_wifi);
    // what melonDS's direct boot leaves
    io_set16(9, 0x04000304u, 0x820F);              // POWCNT1: both screens, both 2D engines, 3D
    io_set16(9, 0x04000300u, 0x0001);              // POSTFLG
    io_set16(7, 0x04000300u, 0x0001);
    io_set16(7, 0x04000304u, 0x0001);              // POWCNT2: the speakers on (the SDK only adds the wireless)
    io_set16(9, 0x04000246u, 0x0300);              // WRAMCNT = 3 (byte 0x247): all the shared WRAM to the ARM7
    io_set16(7, 0x04000240u, 0x0300);              // WRAMSTAT (byte 0x241)
    io_set16(7, 0x04000504u, 0x0200);              // SOUNDBIAS
    io_set16(7, 0x04000206u, 0x0030);              // the wireless wait states
}

// ---- I/O registers ---------------------------------------------------------------------------
static uint16_t r16(int cpu, uint32_t a) {
    uint32_t off = a - 0x04000000u;
    NdsCpu& p = nds_cpu(cpu);
    switch (off) {
    case 0x004: return video_dispstat(cpu);
    case 0x006: return video_vcount();
    case 0x100: case 0x104: case 0x108: case 0x10C: return timer_read(cpu, (int)(off - 0x100) / 4);
    case 0x130: return g_keys;
    case 0x180: case 0x184: return (uint16_t)ipc_read(cpu, a, 2);
    case 0x208: return (uint16_t)p.ime;
    case 0x210: return (uint16_t)p.ie;
    case 0x212: return (uint16_t)(p.ie >> 16);
    case 0x214: return (uint16_t)p.iflags;
    case 0x216: return (uint16_t)(p.iflags >> 16);
    }
    if (off >= 0x1A0 && off < 0x1C0) return (uint16_t)cart_read(cpu, a, 2);
    if (cpu == 7) {
        switch (off) {
        case 0x136: return g_extkeys;
        case 0x138: return rtc_read();
        case 0x1C0: case 0x1C2: return (uint16_t)spi_read(a, 2);
        }
        if (off >= 0x400 && off < 0x520) return (uint16_t)sound_read(a, 2);
    } else {
        if (off >= 0x320 && off < 0x6A4) return (uint16_t)(gx_read(a & ~3u, 4) >> (a & 2) * 8);
    }
    return io_reg16(cpu, a);
}

static void w16(int cpu, uint32_t a, uint16_t v, uint16_t mask) {
    uint32_t off = a - 0x04000000u;
    NdsCpu& p = nds_cpu(cpu);
    uint16_t old = mask == 0xFFFF ? 0 : r16(cpu, a);   // a byte write keeps the other byte as the device has it
    uint16_t m = (uint16_t)((old & ~mask) | (v & mask));
    switch (off) {
    case 0x004: video_dispstat_write(cpu, m); return;
    case 0x006: return;
    case 0x130: return;
    case 0x180: case 0x184: ipc_write(cpu, a, m, 2); return;
    case 0x208: p.ime = m & 1; io_set16(cpu, a, m); nds_irq_update(cpu); return;
    case 0x210: p.ie = (p.ie & 0xFFFF0000u) | m; nds_irq_update(cpu); return;
    case 0x212: p.ie = (p.ie & 0xFFFFu) | (uint32_t)m << 16; nds_irq_update(cpu); return;
    case 0x214: p.iflags &= ~(uint32_t)(v & mask); nds_irq_update(cpu); return;
    case 0x216: p.iflags &= ~((uint32_t)(v & mask) << 16); nds_irq_update(cpu); return;
    }
    if (off >= 0x0B0 && off < 0x0E0) {             // DMA: the control half starts it
        io_set16(cpu, a, m);
        if ((off - 0x0B0) % 12 == 10) dma_write(cpu, (int)(off - 0x0B0) / 12);
        return;
    }
    if (off >= 0x100 && off < 0x110) { timer_write(cpu, (int)(off - 0x100) / 4, (off >> 1) & 1, m); return; }
    if (off >= 0x1A0 && off < 0x1C0) { cart_write(cpu, a, m, 2); return; }
    if (cpu == 7) {
        switch (off) {
        case 0x138: rtc_write(m); return;
        case 0x1C0: case 0x1C2: spi_write(a, m, 2); return;
        case 0x300:
            io_set16(cpu, a, m);
            if (mask & 0xFF00) {                    // HALTCNT
                uint8_t h = (uint8_t)(v >> 8);
                if (h & 0x80) nds_halt(p.c);        // 0x80 halt; 0xC0 sleep (waits the same way)
            }
            return;
        }
        if (off >= 0x400 && off < 0x520) { sound_write(a, m, 2); return; }
    } else {
        if (off >= 0x240 && off < 0x24A) {          // VRAMCNT_A-G, WRAMCNT, VRAMCNT_H-I
            io_set16(cpu, a, m);
            for (int b = 0; b < 2; ++b) {
                if (!(mask >> (8 * b) & 0xFF)) continue;
                uint32_t r = off + b;
                uint8_t x = (uint8_t)(m >> (8 * b));
                if (r == 0x247) {
                    if ((x & 3) != 3) nds_fatal("WRAMCNT = %u: the runtime keeps the shared WRAM with the ARM7", x & 3);
                } else {
                    vram_cnt_write(r < 0x247 ? (int)(r - 0x240) : (int)(r - 0x241), x);
                }
            }
            return;
        }
        if (off >= 0x320 && off < 0x6A4) { gx_write(a, m, 2); return; }
    }
    io_set16(cpu, a, m);
    if (cpu == 9 && (off & ~0x1000u) < 0x70) {
        if (off == 0x066 && (m & 0x8000)) nds_once("2d", 4, "2D: display capture is not done");
        gpu2d_written(off);
    }
}

static uint32_t io_read(int cpu, uint32_t a, int size) {
    uint32_t off = a - 0x04000000u;
    if (off == 0x100000) return ipc_read(cpu, a, 4);
    if (off == 0x100010) return cart_data_word(cpu);
    if (cpu == 7 && off >= 0x800000) {
        uint32_t o = a & 0xFFFF & ~(uint32_t)(size - 1), v = 0;
        std::memcpy(&v, &g_wifi[o], size);
        return v;
    }
    if (off >= 0x2000) {
        nds_once("io read", a, "ARM%d: %d-byte read of %08X, outside the I/O registers", cpu, size, a);
        return 0;
    }
    if (cpu == 9 && off >= 0x320 && off < 0x6A4 && size == 4) return gx_read(a, 4);
    if (size == 4) return r16(cpu, a) | (uint32_t)r16(cpu, a + 2) << 16;
    uint16_t h = r16(cpu, a & ~1u);
    return size == 2 ? h : (uint32_t)(h >> (a & 1) * 8) & 0xFF;
}

static void io_write(int cpu, uint32_t a, uint32_t v, int size) {
    uint32_t off = a - 0x04000000u;
    if (cpu == 7 && off >= 0x800000) {
        uint32_t o = a & 0xFFFF & ~(uint32_t)(size - 1);
        std::memcpy(&g_wifi[o], &v, size);
        return;
    }
    if (off >= 0x2000) {
        nds_once("io write", a, "ARM%d: %d-byte write of %08X to %08X, outside the I/O registers", cpu, size, v, a);
        return;
    }
    if (off == 0x188) { ipc_write(cpu, a, v, size); return; }
    if (off >= 0x1A4 && off < 0x1B0 && size == 4) { cart_write(cpu, a, v, 4); return; }
    if (cpu == 9) {
        if (off >= 0x400 && off < 0x600) { gx_write(a, v, size); return; }   // GXFIFO and the command ports
        if (off >= 0x280 && off < 0x2C0) {           // DIV and SQRT
            uint32_t w = a & ~3u, o = (a & 3) * 8, mk = size == 4 ? ~0u : ((1u << size * 8) - 1) << o;
            io_set(cpu, w, (io_reg(cpu, w) & ~mk) | (v << o & mk));
            math_write(w);
            return;
        }
    }
    if (size == 4) {
        w16(cpu, a, (uint16_t)v, 0xFFFF);
        w16(cpu, a + 2, (uint16_t)(v >> 16), 0xFFFF);
    } else if (size == 2) {
        w16(cpu, a, (uint16_t)v, 0xFFFF);
    } else {
        int sh = (a & 1) * 8;
        w16(cpu, a & ~1u, (uint16_t)(v << sh), (uint16_t)(0xFF << sh));
    }
}

// ---- the rest of the map ----------------------------------------------------------------------
static uint32_t rd(const uint8_t* p, int size) {
    uint32_t v = 0;
    std::memcpy(&v, p, size);
    return v;
}

static uint32_t vram_read(int cpu, uint32_t a, int size) {
    uint32_t v = 0;
    for (int k = 0;; ++k) {
        uint8_t* p = vram_page(cpu, a, k);
        if (!p) break;
        v |= rd(p, size);
    }
    return v;
}

static void vram_write(int cpu, uint32_t a, uint32_t v, int size) {
    bool any = false;
    for (int k = 0;; ++k) {
        uint8_t* p = vram_page(cpu, a, k);
        if (!p) break;
        std::memcpy(p, &v, size);
        any = true;
    }
    if (!any) nds_once("vram", a >> 14, "ARM%d: write to unmapped VRAM %08X", cpu, a);
}

uint32_t nds_mmio_read(int cpu, uint32_t a, int size) {
    switch (a >> 24) {
    case 0x04: return io_read(cpu, a, size);
    case 0x05: if (cpu == 9) return rd(&g_palette[a & 0x7FF], size); break;
    case 0x06: return vram_read(cpu, a, size);
    case 0x07: if (cpu == 9) return rd(&g_oam[a & 0x7FF], size); break;
    case 0x08: case 0x09: return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;   // no GBA cartridge
    case 0x0A: return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    case 0x03: if (cpu == 9) return 0; break;      // the shared WRAM is all the ARM7's
    }
    if (cpu == 9 && a >= 0xFFFF0000u) {
        // the Nintendo logo, which the BIOS keeps at 0x20 as the cartridge's header does at 0xC0
        uint32_t o = a - 0xFFFF0020u;
        if (o + size <= 0x9C) {
            uint32_t v = 0;
            std::memcpy(&v, &cart_rom()[0xC0 + o], size);
            return v;
        }
        nds_once("bios", a, "ARM9: read of the BIOS at %08X", a);
        return 0;
    }
    if (cpu == 7 && a < 0x4000) {
        nds_once("bios", a, "ARM7: read of the BIOS at %08X", a);
        return 0;
    }
    nds_once("unmapped read", a, "ARM%d: %d-byte read of %08X (unmapped)", cpu, size, a);
    return 0;
}

void nds_mmio_write(int cpu, uint32_t a, uint32_t v, int size) {
    switch (a >> 24) {
    case 0x04: io_write(cpu, a, v, size); return;
    case 0x05: if (cpu == 9) { if (size > 1) std::memcpy(&g_palette[a & 0x7FF], &v, size); return; } break;
    case 0x06: vram_write(cpu, a, v, size); return;
    case 0x07: if (cpu == 9) { if (size > 1) std::memcpy(&g_oam[a & 0x7FF], &v, size); return; } break;
    case 0x08: case 0x09: case 0x0A: return;
    }
    nds_once("unmapped write", a, "ARM%d: %d-byte write of %08X to %08X (unmapped)", cpu, size, v, a);
}
