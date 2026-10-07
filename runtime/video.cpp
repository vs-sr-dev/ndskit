// ndskit runtime — the display's timing, and VRAM with its banks.
//
// Lines: 263 of 355 dots, 6 system cycles a dot; HBlank from dot 256; VBlank
// from line 192 to 261 (the flag clears at 262). Each CPU has its DISPSTAT
// (its interrupt enables and VCount setting) and gets its VBlank, HBlank and
// VCount interrupts and its VBlank and HBlank DMA. The 2D engines draw each
// visible line as it starts (gpu2d.cpp); the geometry engine's swap waits
// for the VBlank, and the rendering engine takes its lists at line 215, when
// the DS starts drawing the next frame's 3D (gx.cpp).
//
// VRAM is nine banks (A-I, 656 KiB). VRAMCNT_A-I map each to the LCDC area,
// the 2D engines' backgrounds and sprites, the 3D engine's texture and
// texture-palette slots, the extended palettes or the ARM7. The CPUs and the
// engines reach the banks through a table of 16 KiB pages per area; where
// banks overlap a read ORs them and a write goes to all.
#include "nds.h"
#include <cstring>

// ---- timing ---------------------------------------------------------------------------------
static uint16_t g_vcount;
static uint16_t g_dispstat[2];                      // the written bits of each CPU's DISPSTAT
static uint16_t g_flags;                            // VBlank (0), HBlank (1)
static uint64_t g_line_start;
static const int kRenderLine = 215;                 // the 3D's rendering starts (melonDS's choice)

static int ci(int cpu) { return cpu == 9 ? 0 : 1; }

uint16_t video_vcount() { return g_vcount; }

static bool vmatch(int cpu) {
    uint16_t d = g_dispstat[ci(cpu)];
    return g_vcount == ((d >> 8) | (d & 0x80) << 1);
}

uint16_t video_dispstat(int cpu) {
    uint16_t v = g_dispstat[ci(cpu)] | g_flags;
    uint64_t now = nds_now();
    if (now >= g_line_start + kHBlankTicks) v |= 2;   // events land at a quantum's end: read the time
    if (vmatch(cpu)) v |= 4;
    return v;
}

void video_dispstat_write(int cpu, uint16_t v) { g_dispstat[ci(cpu)] = v & 0xFFB8; }

static void hblank(uint64_t when) {
    g_flags |= 2;
    for (int cpu : {9, 7})
        if (g_dispstat[ci(cpu)] & 0x10) nds_irq_raise(cpu, 1);
    if (g_vcount < 192) dma_event(9, 2);
    nds_schedule(EV_HBLANK, when - kHBlankTicks + kLineTicks + kHBlankTicks);
}

static void line(uint64_t when) {
    g_line_start = when;
    g_vcount = (uint16_t)((g_vcount + 1) % 263);
    g_flags &= ~2;
    if (g_vcount < 192) {
        gpu2d_line(g_vcount);
    } else if (g_vcount == 192) {
        g_flags |= 1;
        gpu2d_vblank();
        gx_vblank();
        nds_count_vblank();
        for (int cpu : {9, 7}) {
            if (g_dispstat[ci(cpu)] & 0x08) nds_irq_raise(cpu, 0);
            dma_event(cpu, 1);
        }
    } else if (g_vcount == kRenderLine) {
        gx_render_start();
    } else if (g_vcount == 262) {
        g_flags &= ~1;
    }
    for (int cpu : {9, 7})
        if (vmatch(cpu) && (g_dispstat[ci(cpu)] & 0x20)) nds_irq_raise(cpu, 2);
    gx_line();
    nds_schedule(EV_LINE, when + kLineTicks);
}

// ---- VRAM -----------------------------------------------------------------------------------
static uint8_t g_vram[0xA4000];
static const uint32_t kBankOff[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
static const uint32_t kBankSize[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
static uint8_t g_cnt[9];

const int g_vram_pages[VRAM_NAREAS] = {32, 8, 16, 8, 41, 16, 32, 6, 2, 2, 1, 1};
VramPage g_vram_map[VRAM_NAREAS][41];

static void map(int area, uint32_t at, int bank) {
    for (uint32_t o = 0; o < kBankSize[bank]; o += 0x4000) {
        int pg = (int)((at + o) >> 14);
        if (pg >= g_vram_pages[area]) return;
        VramPage& P = g_vram_map[area][pg];
        if (P.n < 4) P.p[P.n++] = g_vram + kBankOff[bank] + o;
    }
}

static void rebuild() {
    std::memset(g_vram_map, 0, sizeof g_vram_map);
    uint8_t stat7 = 0;
    for (int b = 0; b < 9; ++b) {
        uint8_t v = g_cnt[b];
        if (!(v & 0x80)) continue;
        int mst = v & ((b <= 1 || b >= 7) ? 3 : 7), ofs = (v >> 3) & 3;
        if (mst == 0) { map(VRAM_LCDC, kBankOff[b], b); continue; }
        switch (b) {
        case 0: case 1:
            if (mst == 1) map(VRAM_BGA, 0x20000u * ofs, b);
            else if (mst == 2) map(VRAM_OBJA, 0x20000u * (ofs & 1), b);
            else map(VRAM_TEX, 0x20000u * ofs, b);
            break;
        case 2: case 3:
            if (mst == 1) map(VRAM_BGA, 0x20000u * ofs, b);
            else if (mst == 2) { map(VRAM_ARM7, 0x20000u * (ofs & 1), b); stat7 |= (uint8_t)(1 << (b - 2)); }
            else if (mst == 3) map(VRAM_TEX, 0x20000u * ofs, b);
            else if (mst == 4) map(b == 2 ? VRAM_BGB : VRAM_OBJB, 0, b);
            break;
        case 4:
            if (mst == 1) map(VRAM_BGA, 0, b);
            else if (mst == 2) map(VRAM_OBJA, 0, b);
            else if (mst == 3) map(VRAM_TEXPAL, 0, b);
            else if (mst == 4) map(VRAM_BGEXTA, 0, b);   // slots 0-3 (its first 32 KiB)
            break;
        case 5: case 6: {
            uint32_t at = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);
            if (mst == 1) map(VRAM_BGA, at, b);
            else if (mst == 2) map(VRAM_OBJA, at, b);
            else if (mst == 3) map(VRAM_TEXPAL, 0x4000u * ((ofs & 1) + (ofs >> 1) * 4), b);
            else if (mst == 4) map(VRAM_BGEXTA, 0x4000u * (ofs & 1), b);   // slots 0-1 or 2-3
            else if (mst == 5) map(VRAM_OBJEXTA, 0, b);
            break;
        }
        case 7:
            if (mst == 1) { map(VRAM_BGB, 0, b); map(VRAM_BGB, 0x10000, b); }
            else if (mst == 2) map(VRAM_BGEXTB, 0, b);
            break;
        case 8:
            if (mst == 1) map(VRAM_BGB, 0x8000, b);
            else if (mst == 2) for (uint32_t o = 0; o < 0x20000; o += 0x4000) map(VRAM_OBJB, o, b);
            else if (mst == 3) map(VRAM_OBJEXTB, 0, b);
            break;
        }
    }
    io_set16(7, 0x04000240u, (uint16_t)(stat7 | (io_reg16(7, 0x04000240u) & 0xFF00)));
}

void vram_cnt_write(int bank, uint8_t v) {
    if (g_cnt[bank] == v) return;
    g_cnt[bank] = v;
    nds_trace("VRAMCNT_%c = %02X", 'A' + bank, v);
    rebuild();
}

uint8_t* vram_page(int cpu, uint32_t a, int which) {
    int area, pg;
    if (cpu == 7) {
        area = VRAM_ARM7; pg = (int)((a & 0x3FFFF) >> 14);
    } else {
        switch ((a >> 21) & 7) {
        case 0: area = VRAM_BGA; pg = (int)((a & 0x7FFFF) >> 14); break;
        case 1: area = VRAM_BGB; pg = (int)((a & 0x1FFFF) >> 14); break;
        case 2: area = VRAM_OBJA; pg = (int)((a & 0x3FFFF) >> 14); break;
        case 3: area = VRAM_OBJB; pg = (int)((a & 0x1FFFF) >> 14); break;
        default: area = VRAM_LCDC; pg = (int)((a & 0xFFFFF) >> 14); if (pg >= 41) return nullptr;
        }
    }
    const VramPage& P = g_vram_map[area][pg];
    return which < P.n ? P.p[which] + (a & 0x3FFF) : nullptr;
}

void vram_copy(int area, uint8_t* dst, uint32_t size) {
    for (uint32_t o = 0; o < size; o += 0x4000) {
        const VramPage& P = g_vram_map[area][o >> 14];
        uint32_t n = size - o < 0x4000 ? size - o : 0x4000;
        if (P.n == 0) { std::memset(dst + o, 0, n); continue; }
        std::memcpy(dst + o, P.p[0], n);
        for (int k = 1; k < P.n; ++k)
            for (uint32_t i = 0; i < n; ++i) dst[o + i] |= P.p[k][i];
    }
}

void video_reset() {
    g_vcount = 0;
    g_line_start = 0;
    nds_event_set(EV_HBLANK, hblank);
    nds_event_set(EV_LINE, line);
    nds_schedule(EV_HBLANK, kHBlankTicks);
    nds_schedule(EV_LINE, kLineTicks);
    std::memset(g_cnt, 0, sizeof g_cnt);
    rebuild();
    gpu2d_reset();
    gx_reset();
    gpu2d_line(0);
}
