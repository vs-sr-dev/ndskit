// ndskit runtime — the display's timing, VRAM and its banks, and (for now)
// the geometry engine's registers.
//
// Lines: 263 of 355 dots, 6 system cycles a dot; HBlank from dot 256; VBlank
// from line 192 to 261 (the flag clears at 262). Each CPU has its DISPSTAT
// (its interrupt enables and VCount setting) and gets its VBlank, HBlank and
// VCount interrupts and its VBlank and HBlank DMA.
//
// VRAM is nine banks (A-I, 656 KiB). VRAMCNT_A-I map each to the LCDC area,
// the 2D engines' backgrounds and sprites, the 3D engine's texture and
// texture-palette slots, the extended palettes or the ARM7. The CPUs reach
// the banks through a table of 16 KiB pages per area; where banks overlap a
// read ORs them and a write goes to all.
//
// The geometry engine (phase 5) is only its registers here: GXFIFO and the
// command ports take everything at once, GXSTAT reads an idle engine with
// an empty FIFO.
#include "nds.h"
#include <cstring>

// ---- timing ---------------------------------------------------------------------------------
static uint16_t g_vcount;
static uint16_t g_dispstat[2];                      // the written bits of each CPU's DISPSTAT
static uint16_t g_flags;                            // VBlank (0), HBlank (1)
static uint64_t g_line_start;
static uint32_t g_gxstat;

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
    if (g_vcount == 192) {
        g_flags |= 1;
        nds_count_vblank();
        for (int cpu : {9, 7}) {
            if (g_dispstat[ci(cpu)] & 0x08) nds_irq_raise(cpu, 0);
            dma_event(cpu, 1);
        }
    } else if (g_vcount == 262) {
        g_flags &= ~1;
    }
    for (int cpu : {9, 7})
        if (vmatch(cpu) && (g_dispstat[ci(cpu)] & 0x20)) nds_irq_raise(cpu, 2);
    uint32_t irqmode = g_gxstat >> 30;
    if (irqmode == 1 || irqmode == 2) nds_irq_raise(9, 21);   // the FIFO is always empty
    nds_schedule(EV_LINE, when + kLineTicks);
}

// ---- VRAM -----------------------------------------------------------------------------------
static uint8_t g_vram[0xA4000];
static const uint32_t kBankOff[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
static const uint32_t kBankSize[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
static uint8_t g_cnt[9];

// The areas, in 16 KiB pages, each page up to four banks' pages.
enum { AREA_BGA, AREA_BGB, AREA_OBJA, AREA_OBJB, AREA_LCDC, AREA_ARM7, AREA_TEX, AREA_TEXPAL, NAREAS };
static const int kPages[NAREAS] = {32, 8, 16, 8, 41, 16, 32, 6};
struct Page { uint8_t* p[4]; int n; };
static Page g_map[NAREAS][41];

static void map(int area, uint32_t at, int bank) {
    for (uint32_t o = 0; o < kBankSize[bank]; o += 0x4000) {
        int pg = (int)((at + o) >> 14);
        if (pg >= kPages[area]) return;
        Page& P = g_map[area][pg];
        if (P.n < 4) P.p[P.n++] = g_vram + kBankOff[bank] + o;
    }
}

static void rebuild() {
    std::memset(g_map, 0, sizeof g_map);
    uint8_t stat7 = 0;
    for (int b = 0; b < 9; ++b) {
        uint8_t v = g_cnt[b];
        if (!(v & 0x80)) continue;
        int mst = v & ((b <= 1 || b >= 7) ? 3 : 7), ofs = (v >> 3) & 3;
        if (mst == 0) { map(AREA_LCDC, kBankOff[b], b); continue; }
        switch (b) {
        case 0: case 1:
            if (mst == 1) map(AREA_BGA, 0x20000u * ofs, b);
            else if (mst == 2) map(AREA_OBJA, 0x20000u * (ofs & 1), b);
            else map(AREA_TEX, 0x20000u * ofs, b);
            break;
        case 2: case 3:
            if (mst == 1) map(AREA_BGA, 0x20000u * ofs, b);
            else if (mst == 2) { map(AREA_ARM7, 0x20000u * (ofs & 1), b); stat7 |= (uint8_t)(1 << (b - 2)); }
            else if (mst == 3) map(AREA_TEX, 0x20000u * ofs, b);
            else if (mst == 4) map(b == 2 ? AREA_BGB : AREA_OBJB, 0, b);
            break;
        case 4:
            if (mst == 1) map(AREA_BGA, 0, b);
            else if (mst == 2) map(AREA_OBJA, 0, b);
            else if (mst == 3) map(AREA_TEXPAL, 0, b);
            break;                                  // 4: BG extended palettes (phase 5)
        case 5: case 6: {
            uint32_t at = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);
            if (mst == 1) map(AREA_BGA, at, b);
            else if (mst == 2) map(AREA_OBJA, at, b);
            else if (mst == 3) map(AREA_TEXPAL, 0x4000u * ((ofs & 1) + (ofs >> 1) * 4), b);
            break;                                  // 4, 5: extended palettes (phase 5)
        }
        case 7: if (mst == 1) { map(AREA_BGB, 0, b); map(AREA_BGB, 0x10000, b); } break;
        case 8:
            if (mst == 1) map(AREA_BGB, 0x8000, b);
            else if (mst == 2) for (uint32_t o = 0; o < 0x20000; o += 0x4000) map(AREA_OBJB, o, b);
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
        area = AREA_ARM7; pg = (int)((a & 0x3FFFF) >> 14);
    } else {
        switch ((a >> 21) & 7) {
        case 0: area = AREA_BGA; pg = (int)((a & 0x7FFFF) >> 14); break;
        case 1: area = AREA_BGB; pg = (int)((a & 0x1FFFF) >> 14); break;
        case 2: area = AREA_OBJA; pg = (int)((a & 0x3FFFF) >> 14); break;
        case 3: area = AREA_OBJB; pg = (int)((a & 0x1FFFF) >> 14); break;
        default: area = AREA_LCDC; pg = (int)((a & 0xFFFFF) >> 14); if (pg >= 41) return nullptr;
        }
    }
    const Page& P = g_map[area][pg];
    return which < P.n ? P.p[which] + (a & 0x3FFF) : nullptr;
}

// ---- the geometry engine's registers ----------------------------------------------------------
static uint8_t g_gx[0x400];                         // 0x04000320-0x0400071F
static uint64_t g_gx_words, g_swaps;

void gx_write(uint32_t a, uint32_t v, int size) {
    uint32_t off = a - 0x04000320u;
    if (a >= 0x04000400u && a < 0x04000600u) {      // GXFIFO and the command ports
        ++g_gx_words;
        if (a == 0x04000540u) ++g_swaps;
        std::memcpy(&g_gx[off & ~3u], &v, 4);
        return;
    }
    if (a == 0x04000600u || a == 0x04000602u) {     // GXSTAT: the FIFO interrupt's mode, the error bit acknowledged
        uint32_t w = size == 4 ? v : a == 0x04000602u ? v << 16 : v;
        if (a == 0x04000602u || size == 4) g_gxstat = (g_gxstat & ~0xC0000000u) | (w & 0xC0000000u);
        uint32_t m = g_gxstat >> 30;
        if (m == 1 || m == 2) nds_irq_raise(9, 21);
        return;
    }
    if (off + size <= sizeof g_gx) std::memcpy(&g_gx[off], &v, size);
}

uint32_t gx_read(uint32_t a, int size) {
    (void)size;
    if (a == 0x04000600u) return 0x06000000u | (g_gxstat & 0xC0000000u);   // idle, FIFO empty, stacks at 0
    if (a == 0x04000604u) return 0;
    uint32_t off = (a - 0x04000320u) & ~3u, v = 0;
    if (off + 4 <= sizeof g_gx) std::memcpy(&v, &g_gx[off], 4);
    return v;
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
}
