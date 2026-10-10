// ndskit runtime — the two 2D engines, in software, a line at a time.
//
// Engine A's registers are at 0x04000000, engine B's at 0x04001000; their
// palettes at 0x05000000 and 0x05000400 (backgrounds, then sprites), OAM at
// 0x07000000 and 0x07000400, their VRAM through the bank areas. A line is
// drawn as it starts, with the registers as they are then (a write in the
// HBlank before it counts, as on the DS).
//
// The layers: backgrounds in text, affine, extended (affine with 16-bit
// entries, 256-colour and direct-colour bitmaps) and large-bitmap forms,
// with 16 or 256 colours and the extended palettes; sprites (OBJ) in tiles,
// 1D and 2D mapping, 16 and 256 colours, affine and double-size, bitmaps,
// semi-transparent and window sprites; windows 0, 1 and the OBJ window;
// blending (alpha, brighter, darker); the master brightness; the display
// modes "off" (white), graphics and VRAM display. On engine A, BG0 can be
// the 3D layer: its pixels are left to the compositor (nds.h's two words),
// which has the 3D picture at the window's resolution. Mosaic is not drawn.
//
// Colours are kept as the DS keeps them inside, 6 bits a component: a
// palette's 5-bit component c is 2c.
#include "nds.h"
#include <cstring>

Gpu2dFrame g_2d;

static int32_t g_ref[2][2][2];                      // [engine][BG2, BG3][x, y]: the affine reference points

static uint32_t reg32(uint32_t a) { return io_reg(9, a); }
static uint16_t reg16(uint32_t a) { return io_reg16(9, a); }

static int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

static void reload_refs(int e) {
    uint32_t io = e ? 0x04001000u : 0x04000000u;
    for (int k = 0; k < 2; ++k) {
        g_ref[e][k][0] = sext28(reg32(io + 0x28 + 0x10 * k));
        g_ref[e][k][1] = sext28(reg32(io + 0x2C + 0x10 * k));
    }
}

void gpu2d_reset() {
    std::memset(&g_2d, 0, sizeof g_2d);
    std::memset(g_ref, 0, sizeof g_ref);
}

void gpu2d_written(uint32_t off) {
    int e = off >= 0x1000 ? 1 : 0;
    uint32_t r = off & 0xFFF;
    if (r >= 0x28 && r < 0x30) {
        int k = 0, xy = (r - 0x28) >> 2;
        g_ref[e][k][xy] = sext28(reg32((e ? 0x04001000u : 0x04000000u) + (r & ~3u)));
    } else if (r >= 0x38 && r < 0x40) {
        int k = 1, xy = (r - 0x38) >> 2;
        g_ref[e][k][xy] = sext28(reg32((e ? 0x04001000u : 0x04000000u) + (r & ~3u)));
    }
}

void gpu2d_vblank() {
    reload_refs(0);
    reload_refs(1);
}

bool gpu2d_top_is_a() { return io_reg16(9, 0x04000304u) & 0x8000; }

// ---- colours ---------------------------------------------------------------------------------
static inline uint32_t c666(uint16_t c) {
    return (uint32_t)(c & 0x1F) << 1 | (uint32_t)(c >> 5 & 0x1F) << 7 | (uint32_t)(c >> 10 & 0x1F) << 13;
}
static inline uint32_t blend(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
    uint32_t out = 0;
    for (int s = 0; s < 18; s += 6) {
        uint32_t v = ((a >> s & 63) * eva + (b >> s & 63) * evb + 8) >> 4;
        out |= (v > 63 ? 63 : v) << s;
    }
    return out;
}
static inline uint32_t brighter(uint32_t a, uint32_t evy) {
    uint32_t out = 0;
    for (int s = 0; s < 18; s += 6) {
        uint32_t c = a >> s & 63;
        out |= (c + ((63 - c) * evy >> 4)) << s;
    }
    return out;
}
static inline uint32_t darker(uint32_t a, uint32_t evy) {
    uint32_t out = 0;
    for (int s = 0; s < 18; s += 6) {
        uint32_t c = a >> s & 63;
        out |= (c - ((c * evy + 7) >> 4)) << s;
    }
    return out;
}

// ---- one engine's line ------------------------------------------------------------------------
// A layer's pixel: the colour (RGB666) and what it is.
enum : uint32_t {
    PX_OPAQUE = 1u << 31,
    PX_3D = 1u << 30,                               // the 3D layer: its colour is the compositor's
    PX_SEMI = 1u << 29,                             // a semi-transparent sprite
    PX_BMPA = 1u << 28,                             // a bitmap sprite: its alpha in bits 24-27
};

struct Line {
    int e;
    uint32_t io, dispcnt;
    int abg, aobj, abgext, aobjext;
    const uint8_t* pal;                             // the engine's BG palette, then its OBJ palette at +0x200
    const uint8_t* oam;
    uint8_t win[256];                               // the layers each pixel's window lets through (bits 0-5)
    uint32_t obj[256];                              // the sprites' line: pixel, PX_OPAQUE if any
    uint8_t objprio[256];
    uint8_t objwin[256];
    uint32_t top[256], below[256];
    uint8_t toplayer[256], belowlayer[256];         // 0-3 BG, 4 OBJ, 5 backdrop
};

static inline uint16_t pal16(const Line& L, uint32_t i) {
    uint16_t v;
    std::memcpy(&v, L.pal + (i & 0x3FF) * 2, 2);
    return v;
}

static void windows(Line& L, int y) {
    uint32_t d = L.dispcnt;
    if (!(d & 0xE000)) { std::memset(L.win, 0x3F, 256); return; }
    uint16_t winout = reg16(L.io + 0x4A), winin = reg16(L.io + 0x48);
    std::memset(L.win, winout & 0x3F, 256);
    if (d & 0x8000)
        for (int x = 0; x < 256; ++x)
            if (L.objwin[x]) L.win[x] = (uint8_t)(winout >> 8 & 0x3F);
    for (int w = 1; w >= 0; --w) {
        if (!(d & (0x2000 << w))) continue;
        uint16_t h = reg16(L.io + 0x40 + 2 * w), v = reg16(L.io + 0x44 + 2 * w);
        int x1 = h >> 8, x2 = h & 0xFF, y1 = v >> 8, y2 = v & 0xFF;
        bool iny = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (!iny) continue;
        uint8_t m = (uint8_t)(winin >> (8 * w) & 0x3F);
        for (int x = 0; x < 256; ++x) {
            bool in = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
            if (in) L.win[x] = m;
        }
    }
}

// A text background's line.
static void bg_text(const Line& L, int bg, int y, uint32_t* out) {
    uint16_t cnt = reg16(L.io + 0x08 + 2 * bg);
    uint32_t hofs = reg16(L.io + 0x10 + 4 * bg) & 0x1FF, vofs = reg16(L.io + 0x12 + 4 * bg) & 0x1FF;
    int size = cnt >> 14;
    uint32_t w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    uint32_t yy = (y + vofs) & (h - 1);
    uint32_t charbase = ((cnt >> 2) & 15) * 0x4000u, screenbase = ((cnt >> 8) & 31) * 0x800u;
    if (L.e == 0) { charbase += ((L.dispcnt >> 24) & 7) * 0x10000u; screenbase += ((L.dispcnt >> 27) & 7) * 0x10000u; }
    bool c256 = cnt & 0x80;
    bool ext = c256 && (L.dispcnt & 0x40000000u);
    uint32_t slot = (uint32_t)bg + ((bg < 2 && (cnt & 0x2000)) ? 2 : 0);
    uint32_t rowblock = (yy >> 8) * (w == 512 ? 2 : 1);
    for (int x = 0; x < 256; ++x) {
        uint32_t xx = (x + hofs) & (w - 1);
        uint32_t se = screenbase + (rowblock + (xx >> 8)) * 0x800 + ((yy & 0xFF) >> 3) * 64 + ((xx & 0xFF) >> 3) * 2;
        uint16_t entry = vram_r16(L.abg, se);
        uint32_t tile = entry & 0x3FF, px = (xx & 7) ^ ((entry & 0x400) ? 7 : 0), py = (yy & 7) ^ ((entry & 0x800) ? 7 : 0);
        uint32_t pal = entry >> 12;
        if (c256) {
            uint8_t idx = vram_r8(L.abg, charbase + tile * 64 + py * 8 + px);
            if (!idx) { out[x] = 0; continue; }
            uint16_t c = ext ? vram_r16(L.abgext, slot * 0x2000 + pal * 512 + idx * 2) : pal16(L, idx);
            out[x] = c666(c) | PX_OPAQUE;
        } else {
            uint8_t b = vram_r8(L.abg, charbase + tile * 32 + py * 4 + px / 2);
            uint8_t idx = (px & 1) ? b >> 4 : b & 15;
            if (!idx) { out[x] = 0; continue; }
            out[x] = c666(pal16(L, pal * 16 + idx)) | PX_OPAQUE;
        }
    }
}

// An affine background's line (the reference point already in place); kind:
// 0 affine (8-bit entries), 1 extended with 16-bit entries, 2 256-colour
// bitmap, 3 direct-colour bitmap, 4 the large bitmap.
static void bg_affine(const Line& L, int bg, int kind, uint32_t* out) {
    uint16_t cnt = reg16(L.io + 0x08 + 2 * bg);
    uint32_t pr = L.io + 0x20 + 0x10 * (bg - 2);
    int32_t pa = (int16_t)reg16(pr), pc = (int16_t)reg16(pr + 4);
    int32_t rx = g_ref[L.e][bg - 2][0], ry = g_ref[L.e][bg - 2][1];
    int size = cnt >> 14;
    bool wrap = cnt & 0x2000;
    uint32_t w, h;
    if (kind <= 1) w = h = 128u << size;
    else if (kind == 4) { w = (size & 1) ? 1024 : 512; h = (size & 1) ? 512 : 1024; }
    else {
        static const uint32_t bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[size]; h = bh[size];
    }
    uint32_t charbase = ((cnt >> 2) & 15) * 0x4000u, screenbase = ((cnt >> 8) & 31) * 0x800u;
    if (L.e == 0 && kind <= 1) { charbase += ((L.dispcnt >> 24) & 7) * 0x10000u; screenbase += ((L.dispcnt >> 27) & 7) * 0x10000u; }
    uint32_t bmpbase = ((cnt >> 8) & 31) * 0x4000u;
    bool ext = L.dispcnt & 0x40000000u;
    for (int x = 0; x < 256; ++x, rx += pa, ry += pc) {
        int32_t tx = rx >> 8, ty = ry >> 8;
        if (wrap) { tx &= (int32_t)w - 1; ty &= (int32_t)h - 1; }
        else if (tx < 0 || ty < 0 || tx >= (int32_t)w || ty >= (int32_t)h) { out[x] = 0; continue; }
        uint32_t ux = (uint32_t)tx, uy = (uint32_t)ty;
        switch (kind) {
        case 0: {
            uint8_t tile = vram_r8(L.abg, screenbase + (uy >> 3) * (w >> 3) + (ux >> 3));
            uint8_t idx = vram_r8(L.abg, charbase + tile * 64u + (uy & 7) * 8 + (ux & 7));
            out[x] = idx ? c666(pal16(L, idx)) | PX_OPAQUE : 0;
            break;
        }
        case 1: {
            uint16_t entry = vram_r16(L.abg, screenbase + ((uy >> 3) * (w >> 3) + (ux >> 3)) * 2);
            uint32_t px = (ux & 7) ^ ((entry & 0x400) ? 7 : 0), py = (uy & 7) ^ ((entry & 0x800) ? 7 : 0);
            uint8_t idx = vram_r8(L.abg, charbase + (entry & 0x3FFu) * 64 + py * 8 + px);
            if (!idx) { out[x] = 0; break; }
            uint16_t c = ext ? vram_r16(L.abgext, (uint32_t)bg * 0x2000 + (entry >> 12) * 512u + idx * 2u) : pal16(L, idx);
            out[x] = c666(c) | PX_OPAQUE;
            break;
        }
        case 2: case 4: {
            uint8_t idx = vram_r8(L.abg, (kind == 2 ? bmpbase : 0) + uy * w + ux);
            out[x] = idx ? c666(pal16(L, idx)) | PX_OPAQUE : 0;
            break;
        }
        case 3: {
            uint16_t c = vram_r16(L.abg, bmpbase + (uy * w + ux) * 2);
            out[x] = (c & 0x8000) ? c666(c) | PX_OPAQUE : 0;
            break;
        }
        }
    }
}

// The sprites' line.
static void sprites(Line& L, int y) {
    std::memset(L.obj, 0, sizeof L.obj);
    std::memset(L.objprio, 4, sizeof L.objprio);
    std::memset(L.objwin, 0, sizeof L.objwin);
    if (!(L.dispcnt & 0x1000)) return;
    static const uint8_t kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const uint8_t kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    uint32_t d = L.dispcnt;
    bool ext = d & 0x80000000u;
    for (int i = 0; i < 128; ++i) {
        uint16_t a0, a1, a2;
        std::memcpy(&a0, L.oam + i * 8, 2);
        std::memcpy(&a1, L.oam + i * 8 + 2, 2);
        std::memcpy(&a2, L.oam + i * 8 + 4, 2);
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = kW[shape][sz], h = kH[shape][sz];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int dy = (y - (a0 & 0xFF)) & 0xFF;
        if (dy >= bh) continue;
        int x0 = a1 & 0x1FF;
        if (x0 >= 256) x0 -= 512;
        int mode = (a0 >> 10) & 3;
        if (a0 & 0x1000) nds_once("2d", 1, "2D: a mosaic sprite (mosaic is not drawn)");
        uint32_t prio = (a2 >> 10) & 3;
        bool c256 = a0 & 0x2000;
        uint32_t tile = a2 & 0x3FF;
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            uint32_t g = ((a1 >> 9) & 31) * 32;
            int16_t v;
            std::memcpy(&v, L.oam + g + 6, 2); pa = v;
            std::memcpy(&v, L.oam + g + 14, 2); pb = v;
            std::memcpy(&v, L.oam + g + 22, 2); pc = v;
            std::memcpy(&v, L.oam + g + 30, 2); pd = v;
        }
        uint32_t alpha = a2 >> 12;
        if (mode == 3 && alpha == 0) continue;
        for (int sx = 0; sx < bw; ++sx) {
            int X = x0 + sx;
            if (X < 0 || X > 255) continue;
            int tx, ty;
            if (affine) {
                int cx = sx - bw / 2, cy = dy - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                ty = (a1 & 0x2000) ? h - 1 - dy : dy;
            }
            uint32_t px;
            if (mode == 3) {
                uint32_t addr;
                if (d & 0x40) addr = tile * (128u << ((d >> 22) & 1)) + ((uint32_t)ty * w + tx) * 2;
                else {
                    bool wide = d & 0x20;
                    uint32_t mask = wide ? 0x1F : 0x0F;
                    addr = (tile & mask) * 0x10 + (tile & ~mask) * 0x80 + (uint32_t)ty * (wide ? 512 : 256) + tx * 2;
                }
                uint16_t c = vram_r16(L.aobj, addr);
                if (!(c & 0x8000)) continue;
                px = c666(c) | PX_OPAQUE | PX_BMPA | alpha << 24;
            } else {
                uint32_t addr;
                uint32_t tbytes = c256 ? 64 : 32;
                if (d & 0x10) addr = tile * (32u << ((d >> 20) & 3)) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * tbytes;
                else addr = tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * tbytes;
                uint32_t idx;
                if (c256) idx = vram_r8(L.aobj, addr + (ty & 7) * 8 + (tx & 7));
                else {
                    uint8_t b = vram_r8(L.aobj, addr + (ty & 7) * 4 + (tx & 7) / 2);
                    idx = (tx & 1) ? b >> 4 : b & 15;
                }
                if (!idx) continue;
                uint16_t c;
                if (c256) c = ext ? vram_r16(L.aobjext, (a2 >> 12) * 512u + idx * 2) : pal16(L, 0x100 + idx);
                else c = pal16(L, 0x100 + (a2 >> 12) * 16 + idx);
                px = c666(c) | PX_OPAQUE | (mode == 1 ? PX_SEMI : 0);
            }
            if (mode == 2) { L.objwin[X] = 1; continue; }
            if (prio < L.objprio[X]) { L.obj[X] = px; L.objprio[X] = (uint8_t)prio; }
        }
    }
}

static inline void push(Line& L, int x, uint32_t px, int layer) {
    L.below[x] = L.top[x]; L.belowlayer[x] = L.toplayer[x];
    L.top[x] = px; L.toplayer[x] = (uint8_t)layer;
}

static uint32_t g_cap;                              // DISPCAPCNT as the frame began with it enabled; 0: none
// The capture's source A is engine A's graphics (BGs, OBJs, 3D) whatever its display
// shows: on a line shown otherwise (VRAM or main memory display), they are composed here.
static uint32_t g_capline[192][256][2];
static bool g_cap_own[192];                         // line y of the capture comes from g_capline

static void layers(Line& L, int y, uint32_t* outw, uint32_t w1base);

static void draw_line(int e, int y) {
    static Line L;
    L.e = e;
    L.io = e ? 0x04001000u : 0x04000000u;
    L.dispcnt = reg32(L.io);
    L.abg = e ? VRAM_BGB : VRAM_BGA;
    L.aobj = e ? VRAM_OBJB : VRAM_OBJA;
    L.abgext = e ? VRAM_BGEXTB : VRAM_BGEXTA;
    L.aobjext = e ? VRAM_OBJEXTB : VRAM_OBJEXTA;
    L.pal = g_palette + (e ? 0x400 : 0);
    L.oam = g_oam + (e ? 0x400 : 0);
    uint32_t* outw = &g_2d.px[e][y][0][0];
    uint16_t mb = reg16(L.io + 0x6C);
    uint32_t mbmode = (mb >> 14) & 3, mbf = mb & 0x1F;
    if (mbf > 16) mbf = 16;
    uint32_t w1base = (mbmode == 1 || mbmode == 2) ? (mbmode << 24 | mbf << 26) : 0;
    uint32_t d = L.dispcnt;
    uint32_t dmode = (d >> 16) & (e ? 1 : 3);
    uint16_t pow = io_reg16(9, 0x04000304u);
    bool on = pow & (e ? 0x200 : 0x2);
    if (e == 0) g_cap_own[y] = false;
    if (!on || (d & 0x80)) {
        // the engine off or a forced blank: white
        for (int x = 0; x < 256; ++x) { outw[2 * x] = 0x3FFFF; outw[2 * x + 1] = w1base; }
        return;
    }
    if (dmode == 1) {
        layers(L, y, outw, w1base);
        return;
    }
    if (e == 0 && g_cap) {
        layers(L, y, &g_capline[y][0][0], w1base);
        g_cap_own[y] = true;
    }
    if (dmode == 0) {                               // the display off: white
        for (int x = 0; x < 256; ++x) { outw[2 * x] = 0x3FFFF; outw[2 * x + 1] = w1base; }
    } else if (dmode == 2) {                        // VRAM display: a bank in the LCDC area, direct colour
        uint32_t base = ((d >> 18) & 3) * 0x20000u + (uint32_t)y * 512;
        for (int x = 0; x < 256; ++x) {
            outw[2 * x] = c666(vram_r16(VRAM_LCDC, base + 2 * x));
            outw[2 * x + 1] = w1base;
        }
    } else {
        nds_once("2d", 3, "2D: main memory display is not drawn");
        for (int x = 0; x < 256; ++x) { outw[2 * x] = 0; outw[2 * x + 1] = w1base; }
    }
}

// An engine's graphics on line y: its BGs, OBJs (and engine A's 3D) through the
// windows and the blending, two words a pixel into outw.
static void layers(Line& L, int y, uint32_t* outw, uint32_t w1base) {
    int e = L.e;
    uint32_t d = L.dispcnt;
    sprites(L, y);
    windows(L, y);
    uint32_t backdrop = c666(pal16(L, 0));
    for (int x = 0; x < 256; ++x) {
        L.top[x] = L.below[x] = backdrop;
        L.toplayer[x] = L.belowlayer[x] = 5;
    }
    int bgmode = d & 7;
    bool is3d = e == 0 && (d & 8);
    // each BG's kind in this mode: -1 none, 0 text, 1 affine, 2 extended, 3 large bitmap
    static const int8_t kKind[8][4] = {
        {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 0, 2},
        {0, 0, 1, 2}, {0, 0, 2, 2}, {0, -1, 3, -1}, {-1, -1, -1, -1}};
    static uint32_t buf[256];
    for (int prio = 3; prio >= 0; --prio) {
        for (int bg = 3; bg >= 0; --bg) {
            if (!(d & (0x100u << bg))) continue;
            uint16_t cnt = reg16(L.io + 0x08 + 2 * bg);
            if ((int)(cnt & 3) != prio) continue;
            int kind = kKind[bgmode][bg];
            if (kind < 0) continue;
            if (cnt & 0x40) nds_once("2d", 2, "2D: a mosaic background (mosaic is not drawn)");
            if (bg == 0 && is3d) {
                for (int x = 0; x < 256; ++x)
                    if (L.win[x] & 1) push(L, x, PX_OPAQUE | PX_3D, 0);
                continue;
            }
            if (kind == 0) bg_text(L, bg, y, buf);
            else if (kind == 1) bg_affine(L, bg, 0, buf);
            else if (kind == 3) bg_affine(L, bg, 4, buf);
            else {
                int k = !(cnt & 0x80) ? 1 : (cnt & 4) ? 3 : 2;
                bg_affine(L, bg, k, buf);
            }
            for (int x = 0; x < 256; ++x)
                if ((buf[x] & PX_OPAQUE) && (L.win[x] & (1 << bg))) push(L, x, buf[x], bg);
        }
        for (int x = 0; x < 256; ++x)
            if (L.objprio[x] == prio && (L.obj[x] & PX_OPAQUE) && (L.win[x] & 0x10)) push(L, x, L.obj[x], 4);
    }
    // the affine reference points move on a line
    for (int k = 0; k < 2; ++k) {
        uint32_t pr = L.io + 0x20 + 0x10 * k;
        g_ref[e][k][0] += (int16_t)reg16(pr + 2);
        g_ref[e][k][1] += (int16_t)reg16(pr + 6);
    }

    // blending
    uint16_t bldcnt = reg16(L.io + 0x50), bldalpha = reg16(L.io + 0x52);
    uint32_t eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F, evy = reg16(L.io + 0x54) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint32_t bmode = (bldcnt >> 6) & 3;
    for (int x = 0; x < 256; ++x) {
        uint32_t t = L.top[x], b = L.below[x];
        int tl = L.toplayer[x], bl = L.belowlayer[x];
        bool fx = L.win[x] & 0x20;
        bool first = bldcnt & (1u << tl), second = (bldcnt >> 8) & (1u << bl);
        uint32_t w0 = 0, w1 = w1base;
        if (t & PX_3D) {
            // the 3D on top: the compositor blends it with what lies under it
            w0 = (b & 0x3FFFF) | 1u << 24;
            if (second && !(b & PX_3D)) w0 |= 1u << 26;
            if (fx && first && (bmode == 2 || bmode == 3)) w1 |= evy << 16 | bmode << 21;
        } else if (fx && (t & (PX_SEMI | PX_BMPA)) && second) {
            uint32_t a = eva, bb = evb;
            if (t & PX_BMPA) { a = ((t >> 24) & 15) + 1; bb = 16 - a; }
            if (b & PX_3D) { w0 = (t & 0x3FFFF) | 2u << 24; w1 |= a | bb << 8; }
            else w0 = blend(t, b, a, bb);
        } else if (fx && first && bmode == 1 && second) {
            if (b & PX_3D) { w0 = (t & 0x3FFFF) | 2u << 24; w1 |= eva | evb << 8; }
            else w0 = blend(t, b, eva, evb);
        } else if (fx && first && bmode == 2) {
            w0 = brighter(t, evy);
        } else if (fx && first && bmode == 3) {
            w0 = darker(t, evy);
        } else {
            w0 = t & 0x3FFFF;
        }
        outw[2 * x] = w0;
        outw[2 * x + 1] = w1;
    }
}

bool g_2d_on = true;

void gpu2d_line(int y) {
    if (y == 0) {
        uint32_t cnt = io_reg(9, 0x04000064u);
        g_cap = cnt >> 31 ? cnt : 0;
    }
    if (!g_2d_on && !g_cap) return;                 // a capture needs the lines even when nothing is shown
    draw_line(0, y);
    draw_line(1, y);
}

// ---- display capture --------------------------------------------------------------------------
// The frame after DISPCAPCNT's enable bit is set (latched at its line 0) is captured at its end
// into a VRAM bank (A-D, written directly whatever it is mapped to), from source A (engine A's
// graphics before the master brightness, whatever its display shows, or the 3D alone), source B (a VRAM bank, the one
// DISPCNT's display block names, from the read offset) or the two blended by EVA and EVB;
// 15-bit colour with the alpha bit, lines of the capture's width; then the enable bit clears.
// The 3D comes from the renderer at the DS's resolution. Main memory display as source B is
// not done.
static inline uint16_t c555(uint32_t c666) {
    return (uint16_t)((c666 >> 1 & 31) | (c666 >> 7 & 31) << 5 | (c666 >> 13 & 31) << 10);
}

void gpu2d_capture() {
    uint32_t cnt = g_cap;
    g_cap = 0;
    if (!cnt) return;
    static const int kW[4] = {128, 256, 256, 256}, kH[4] = {128, 64, 128, 192};
    int size = cnt >> 20 & 3, w = kW[size], h = kH[size];
    uint32_t eva = cnt & 31, evb = cnt >> 8 & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    int src = cnt >> 29 & 3;
    bool only3d = cnt >> 24 & 1, fifo = cnt >> 25 & 1;
    uint8_t* dst = vram_bank(cnt >> 16 & 3);
    uint32_t woff = (cnt >> 18 & 3) * 0x8000u;
    uint8_t* srcb = vram_bank(reg32(0x04000000u) >> 18 & 3);
    uint32_t roff = (cnt >> 26 & 3) * 0x8000u;
    static uint32_t p3[256 * 192];
    bool have3d = false;
    if (src != 1) {
        have3d = display_read3d(p3);
        if (!have3d) {
            nds_once("2d", 5, "2D: a display capture without the 3D (no OpenGL in this run)");
            std::memset(p3, 0, sizeof p3);
        }
    }
    if (src != 0 && fifo) nds_once("2d", 6, "2D: display capture from main memory display is not done");
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint16_t a = 0, b = 0;
            if (src != 1) {
                uint32_t px = p3[y * 256 + x];
                if (only3d) {
                    a = c555(px) | ((px >> 18 & 31) ? 0x8000 : 0);
                } else {
                    const uint32_t* wd = g_cap_own[y] ? g_capline[y][x] : g_2d.px[0][y][x];
                    uint32_t ww[2] = {wd[0], wd[1] & ~(3u << 24)};
                    uint32_t rgb = gpu2d_compose(ww, px);
                    a = (uint16_t)((rgb >> 19 & 31) | (rgb >> 11 & 31) << 5 | (rgb >> 3 & 31) << 10 | 0x8000);
                }
            }
            if (src != 0 && !fifo) {
                uint32_t o = (roff + (uint32_t)(y * 256 + x) * 2) & 0x1FFFF;
                b = (uint16_t)(srcb[o] | srcb[o + 1] << 8);
            }
            uint16_t out = a;
            if (src == 1) {
                out = b;
            } else if (src >= 2) {
                uint32_t aa = a >> 15, ab = b >> 15;
                out = (uint16_t)((eva && aa) || (evb && ab) ? 0x8000 : 0);
                for (int s = 0; s < 15; s += 5) {
                    uint32_t v = ((a >> s & 31) * eva * aa + (b >> s & 31) * evb * ab + 8) >> 4;
                    out |= (uint16_t)((v > 31 ? 31 : v) << s);
                }
            }
            uint32_t o = (woff + (uint32_t)(y * w + x) * 2) & 0x1FFFF;
            dst[o] = (uint8_t)out;
            dst[o + 1] = (uint8_t)(out >> 8);
        }
    io_set16(9, 0x04000066u, io_reg16(9, 0x04000066u) & 0x7FFF);
}

// ---- the compositor, in software ----------------------------------------------------------------
uint32_t gpu2d_compose(const uint32_t* w, uint32_t px3d) {
    uint32_t w0 = w[0], w1 = w[1], c = w0 & 0x3FFFF;
    uint32_t mode = (w0 >> 24) & 3;
    uint32_t c3 = px3d & 0x3FFFF, a3 = (px3d >> 18) & 31;
    if (mode == 1) {
        if (a3 == 0) {
            // a transparent 3D pixel: what lies under it shows
        } else if (w0 & (1u << 26)) {
            uint32_t eva = a3 + 1, evb = 32 - eva, out = 0;
            for (int s = 0; s < 18; s += 6) out |= (((c3 >> s & 63) * eva + (c >> s & 63) * evb) >> 5) << s;
            c = out;
        } else {
            c = c3;
        }
        if (a3) {
            uint32_t bm = (w1 >> 21) & 3, evy = (w1 >> 16) & 31;
            if (bm == 2) c = brighter(c, evy);
            else if (bm == 3) c = darker(c, evy);
        }
    } else if (mode == 2 && a3) {
        c = blend(c, c3, w1 & 31, (w1 >> 8) & 31);
    }
    uint32_t mm = (w1 >> 24) & 3, mf = (w1 >> 26) & 31;
    if (mm == 1) c = brighter(c, mf);
    else if (mm == 2) c = darker(c, mf);
    uint32_t r = c & 63, g = c >> 6 & 63, b = c >> 12 & 63;
    r = r << 2 | r >> 4; g = g << 2 | g >> 4; b = b << 2 | b >> 4;
    return r << 16 | g << 8 | b;
}
