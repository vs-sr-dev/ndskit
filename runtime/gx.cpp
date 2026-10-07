// ndskit runtime — the geometry engine, in software.
//
// Commands come packed through GXFIFO (0x04000400: a word of up to four
// command bytes, then their parameters) or one at a time through the
// command ports (0x04000440-0x040005CC: each write a parameter). They run at
// once, in fixed point as the DS computes: the matrix stacks (projection,
// position and direction, texture), vertices in their five forms, texture
// coordinates and their transforms, lighting at NORMAL, polygons from
// triangles, quads and their strips, face culling, clipping against the
// view volume (the far plane by POLYGON_ATTR bit 12), the screen position
// through the viewport, BOX_TEST, POS_TEST and VEC_TEST with their results.
//
// SWAP_BUFFERS waits for the VBlank: the commands that follow it wait in a
// queue (GXSTAT shows the engine busy and the FIFO filling), then the
// polygon and vertex lists go to the rendering engine and the queue runs.
// The rendering engine takes its lists, the 3D registers and the texture
// memory at line 215 (render3d.h): what it draws is shown in the next frame.
//
// Not done: the 1-dot polygons' depth test, the lists' limits beyond the
// DS's own (2 048 polygons, 6 144 vertices: past them a polygon is dropped,
// as on the DS, and DISP3DCNT's overflow bit set).
#include "nds.h"
#include "render3d.h"
#include <algorithm>
#include <cstring>
#include <deque>

static uint8_t g_regs[0x400];                       // 0x04000320-0x0400071F, as written

static int32_t mtx_proj[16], mtx_pos[16], mtx_vec[16], mtx_tex[16], mtx_clip[16];
static int32_t stk_proj[16], stk_tex[16], stk_pos[32][16], stk_vec[32][16];
static uint32_t sp_proj, sp_tex, sp_pos;
static bool clip_dirty = true;
static int mtx_mode;
static uint32_t g_stat;                             // GXSTAT's own bits: the test result (1), the stack error (15), IRQ mode (30-31)

// vertex state
static int16_t vtx[3];                              // the last vertex given
static int16_t raw_tc[2], tc[2];                    // texture coordinates as given, as transformed (1.11.4)
static int16_t normal[3];
static uint8_t vcolor[3];                           // 5-bit
static uint32_t attr_pending, attr, tex_param, pltt_base;
static int prim = -1;                               // BEGIN_VTXS's type, -1 outside
static uint8_t viewport[4];                         // x1, y1 (top), x2, y2 (bottom), as the screen counts
// lighting
static int32_t light_vec[4][3];
static uint8_t light_col[4][3];
static uint8_t diffuse[3], ambient[3], specular[3], emission[3];
static bool shine_table_on;
static uint8_t shine_table[128];

// the lists being built, and the rendering engine's
struct Lists { std::vector<GxVertex> verts; std::vector<GxPoly> polys; uint32_t nverts_ds = 0; };
static Lists g_build, g_render;
static bool g_render_wbuf, g_render_manual;
static uint64_t g_swaps;
static bool g_swap_pending;
static uint32_t g_swap_param;
static Render3dJob g_job;

// a strip's or quad's vertices so far (clip coordinates + attributes)
struct V { int32_t p[4]; int32_t c[3]; int32_t t[2]; };
static V g_strip[4];
static int g_nstrip, g_strip_count;

// the FIFO's decoder and the queue behind a swap
static uint32_t g_cmds;                             // the packed command bytes left
static int g_ncmds;
static int g_cur = -1, g_need, g_got;
static uint32_t g_params[32];
struct Queued { uint8_t cmd; uint8_t n; uint32_t p[32]; };
static std::deque<Queued> g_queue;

static const uint8_t kParams[256] = {
    // 0x00
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x10: MTX_MODE PUSH POP STORE RESTORE IDENTITY LOAD44 LOAD43 MULT44 MULT43 MULT33 SCALE TRANS
    1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0,
    // 0x20: COLOR NORMAL TEXCOORD VTX16 VTX10 XY XZ YZ DIFF POLYGON_ATTR TEXIMAGE_PARAM PLTT_BASE
    1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    // 0x30: DIF_AMB SPE_EMI LIGHT_VECTOR LIGHT_COLOR SHININESS
    1, 1, 1, 1, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x40: BEGIN_VTXS END_VTXS
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x50: SWAP_BUFFERS
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x60: VIEWPORT
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x70: BOX_TEST POS_TEST VEC_TEST
    3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static bool known(uint32_t cmd) {
    return cmd == 0 || cmd == 0x11 || cmd == 0x15 || cmd == 0x41 || (cmd < 0x80 && kParams[cmd]);
}

// ---- matrices -----------------------------------------------------------------------------------
static void identity(int32_t* m) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 0x1000;
}

// m = n x m (n given, row-major, rows of four)
static void mult44(int32_t* m, const int32_t* n) {
    int32_t t[16];
    std::memcpy(t, m, 64);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[r * 4 + c] = (int32_t)(((int64_t)n[r * 4] * t[c] + (int64_t)n[r * 4 + 1] * t[4 + c] +
                                      (int64_t)n[r * 4 + 2] * t[8 + c] + (int64_t)n[r * 4 + 3] * t[12 + c]) >> 12);
}
static void mult43(int32_t* m, const int32_t* n) {
    int32_t f[16];
    for (int r = 0; r < 4; ++r) {
        f[r * 4] = n[r * 3]; f[r * 4 + 1] = n[r * 3 + 1]; f[r * 4 + 2] = n[r * 3 + 2];
        f[r * 4 + 3] = r == 3 ? 0x1000 : 0;
    }
    mult44(m, f);
}
static void mult33(int32_t* m, const int32_t* n) {
    int32_t t[16];
    std::memcpy(t, m, 64);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            m[r * 4 + c] = (int32_t)(((int64_t)n[r * 3] * t[c] + (int64_t)n[r * 3 + 1] * t[4 + c] +
                                      (int64_t)n[r * 3 + 2] * t[8 + c]) >> 12);
}
static void scale(int32_t* m, const int32_t* s) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) m[r * 4 + c] = (int32_t)(((int64_t)m[r * 4 + c] * s[r]) >> 12);
}
static void translate(int32_t* m, const int32_t* v) {
    for (int c = 0; c < 4; ++c)
        m[12 + c] = (int32_t)(((int64_t)m[c] * v[0] + (int64_t)m[4 + c] * v[1] + (int64_t)m[8 + c] * v[2] +
                               ((int64_t)m[12 + c] << 12)) >> 12);
}

static void update_clip() {
    if (!clip_dirty) return;
    std::memcpy(mtx_clip, mtx_proj, 64);
    mult44(mtx_clip, mtx_pos);
    clip_dirty = false;
}

// the current matrix (or matrices) of the mode, for loads and multiplies
template <typename F> static void on_current(F f, bool both_in_mode2) {
    switch (mtx_mode) {
    case 0: f(mtx_proj); clip_dirty = true; break;
    case 1: f(mtx_pos); clip_dirty = true; break;
    case 2: f(mtx_pos); if (both_in_mode2) f(mtx_vec); clip_dirty = true; break;
    case 3: f(mtx_tex); break;
    }
}

// ---- lighting -----------------------------------------------------------------------------------
static void lighting() {
    int32_t n[3];
    for (int i = 0; i < 3; ++i)
        n[i] = (int32_t)(((int64_t)normal[0] * mtx_vec[i] + (int64_t)normal[1] * mtx_vec[4 + i] +
                          (int64_t)normal[2] * mtx_vec[8 + i]) >> 12);
    int32_t c[3] = {emission[0], emission[1], emission[2]};
    for (int l = 0; l < 4; ++l) {
        if (!(attr & (1u << l))) continue;
        int32_t dl = -(int32_t)(((int64_t)light_vec[l][0] * n[0] + (int64_t)light_vec[l][1] * n[1] +
                                 (int64_t)light_vec[l][2] * n[2]) >> 10);
        dl = std::clamp(dl, 0, 255);
        int32_t sl = -(int32_t)(((int64_t)(light_vec[l][0] >> 1) * n[0] + (int64_t)(light_vec[l][1] >> 1) * n[1] +
                                 (int64_t)((light_vec[l][2] - 0x200) >> 1) * n[2]) >> 10);
        if (sl < 0) sl = 0;
        else if (sl > 255) sl = (0x100 - sl) & 0xFF;
        sl = ((sl * sl) >> 7) - 0x100;
        if (sl < 0) sl = 0;
        if (shine_table_on) sl = shine_table[sl >> 1];
        for (int i = 0; i < 3; ++i) {
            c[i] += (specular[i] * light_col[l][i] * sl) >> 13;
            c[i] += (diffuse[i] * light_col[l][i] * dl) >> 13;
            c[i] += (ambient[i] * light_col[l][i]) >> 5;
        }
    }
    for (int i = 0; i < 3; ++i) vcolor[i] = (uint8_t)std::min(c[i], 31);
}

// ---- polygons -----------------------------------------------------------------------------------
template <int comp, int plane>
static V clip_point(const V& in, const V& out) {
    int64_t num = (int64_t)in.p[3] - (int64_t)plane * in.p[comp];
    int64_t den = num - ((int64_t)out.p[3] - (int64_t)plane * out.p[comp]);
    V m;
    auto lerp = [&](int32_t a, int32_t b) { return den ? (int32_t)(a + ((int64_t)(b - a) * num) / den) : a; };
    for (int i = 0; i < 4; ++i) m.p[i] = i == comp ? 0 : lerp(in.p[i], out.p[i]);
    m.p[comp] = plane * m.p[3];
    for (int i = 0; i < 3; ++i) m.c[i] = lerp(in.c[i], out.c[i]);
    for (int i = 0; i < 2; ++i) m.t[i] = lerp(in.t[i], out.t[i]);
    return m;
}

// Sutherland-Hodgman against comp <= w (plane +1) then comp >= -w (plane -1)
template <int comp> static int clip_axis(V* v, int n) {
    V tmp[10];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const V& a = v[i];
        const V& b = v[(i + 1) % n];
        bool ain = a.p[comp] <= a.p[3], bin = b.p[comp] <= b.p[3];
        if (ain) { if (m < 10) tmp[m++] = a; }
        if (ain != bin && m < 10) tmp[m++] = ain ? clip_point<comp, 1>(a, b) : clip_point<comp, 1>(b, a);
    }
    n = 0;
    for (int i = 0; i < m; ++i) {
        const V& a = tmp[i];
        const V& b = tmp[(i + 1) % m];
        bool ain = a.p[comp] >= -a.p[3], bin = b.p[comp] >= -b.p[3];
        if (ain) { if (n < 10) v[n++] = a; }
        if (ain != bin && n < 10) v[n++] = ain ? clip_point<comp, -1>(a, b) : clip_point<comp, -1>(b, a);
    }
    return n;
}

static void submit_poly(const V* in, int n) {
    // face culling, on the clip coordinates (melonDS's form: the normal of the
    // triangle (x, y, w) of the first three vertices, against the eye)
    const V &v0 = in[0], &v1 = in[1], &v2 = in[2];
    int64_t nx = (int64_t)(v0.p[1] - v1.p[1]) * (v2.p[3] - v1.p[3]) - (int64_t)(v0.p[3] - v1.p[3]) * (v2.p[1] - v1.p[1]);
    int64_t ny = (int64_t)(v0.p[3] - v1.p[3]) * (v2.p[0] - v1.p[0]) - (int64_t)(v0.p[0] - v1.p[0]) * (v2.p[3] - v1.p[3]);
    int64_t nz = (int64_t)(v0.p[0] - v1.p[0]) * (v2.p[1] - v1.p[1]) - (int64_t)(v0.p[1] - v1.p[1]) * (v2.p[0] - v1.p[0]);
    while ((nx >> 31) ^ (nx >> 63) || (ny >> 31) ^ (ny >> 63) || (nz >> 31) ^ (nz >> 63)) { nx >>= 4; ny >>= 4; nz >>= 4; }
    int64_t dot = (int64_t)v1.p[0] * nx + (int64_t)v1.p[1] * ny + (int64_t)v1.p[3] * nz;
    bool front = dot < 0;
    if (front ? !(attr & 0x80) : dot > 0 ? !(attr & 0x40) : !(attr & 0xC0)) return;

    V v[10];
    for (int i = 0; i < n; ++i) v[i] = in[i];
    if (!(attr & 0x1000))
        for (int i = 0; i < n; ++i)
            if (v[i].p[2] > v[i].p[3]) return;      // past the far plane, and not to be drawn cut
    n = clip_axis<2>(v, n);
    if (n) n = clip_axis<0>(v, n);
    if (n) n = clip_axis<1>(v, n);
    if (n < 3) return;

    if (g_build.polys.size() >= 2048 || g_build.nverts_ds + (uint32_t)n > 6144) {
        io_set16(9, 0x04000060u, (uint16_t)(io_reg16(9, 0x04000060u) | 0x2000));
        nds_once("gx", 1, "3D: the polygon or vertex list is full (DS limits): polygons dropped");
        return;
    }
    GxPoly P{};
    P.first = (uint32_t)g_build.verts.size();
    P.n = (uint32_t)n;
    P.attr = attr;
    P.tex = tex_param;
    P.pal = pltt_base;
    uint32_t fmt = (tex_param >> 26) & 7, alpha = (attr >> 16) & 31;
    P.translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
    int vx = viewport[0], vy = viewport[1];
    int vw = (viewport[2] - viewport[0] + 1) & 0x1FF, vh = (viewport[3] - viewport[1] + 1) & 0xFF;
    int ytop = 192, ybot = 0;
    for (int i = 0; i < n; ++i) {
        GxVertex g;
        g.x = v[i].p[0]; g.y = v[i].p[1]; g.z = v[i].p[2]; g.w = v[i].p[3];
        int64_t w = g.w;
        if (w > 0) {
            g.ix = (int16_t)(((int64_t)(g.x + w) * vw) / (w * 2) + vx);
            g.iy = (int16_t)(((int64_t)(-g.y + w) * vh) / (w * 2) + vy);
            g.sx = (float)((double)(g.x + w) * vw / (double)(w * 2) + vx);
            g.sy = (float)((double)(-g.y + w) * vh / (double)(w * 2) + vy);
        } else {
            g.ix = (int16_t)vx; g.iy = (int16_t)vy; g.sx = (float)vx; g.sy = (float)vy;
        }
        g.r = v[i].c[0] / 16.0f; g.g = v[i].c[1] / 16.0f; g.b = v[i].c[2] / 16.0f;
        g.s = v[i].t[0] / 16.0f; g.t = v[i].t[1] / 16.0f;
        ytop = std::min(ytop, (int)g.iy);
        ybot = std::max(ybot, (int)g.iy);
        g_build.verts.push_back(g);
    }
    ytop = std::clamp(ytop, 0, 255);
    ybot = std::clamp(ybot, 0, 255);
    P.sortkey = (uint32_t)ybot << 8 | (uint32_t)ytop;
    g_build.polys.push_back(P);
    g_build.nverts_ds += (uint32_t)n;
}

static void add_vertex() {
    if (prim < 0) return;
    if (tex_param >> 30 == 3) {                     // texture coordinates from the vertex
        for (int i = 0; i < 2; ++i)
            tc[i] = (int16_t)(((int64_t)vtx[0] * mtx_tex[i] + (int64_t)vtx[1] * mtx_tex[4 + i] +
                               (int64_t)vtx[2] * mtx_tex[8 + i] + ((int64_t)raw_tc[i] << 24)) >> 24);
    }
    update_clip();
    V nv;
    for (int i = 0; i < 4; ++i)
        nv.p[i] = (int32_t)(((int64_t)vtx[0] * mtx_clip[i] + (int64_t)vtx[1] * mtx_clip[4 + i] +
                             (int64_t)vtx[2] * mtx_clip[8 + i] + (int64_t)0x1000 * mtx_clip[12 + i]) >> 12);
    for (int i = 0; i < 3; ++i) nv.c[i] = vcolor[i] << 4 | (vcolor[i] ? 0xF : 0);   // 9 bits, as the DS keeps them
    nv.t[0] = tc[0]; nv.t[1] = tc[1];
    ++g_strip_count;
    switch (prim) {
    case 0:                                         // triangles
        g_strip[g_nstrip++] = nv;
        if (g_nstrip == 3) { submit_poly(g_strip, 3); g_nstrip = 0; }
        break;
    case 1:                                         // quads
        g_strip[g_nstrip++] = nv;
        if (g_nstrip == 4) { submit_poly(g_strip, 4); g_nstrip = 0; }
        break;
    case 2:                                         // triangle strip: every other one turned round
        if (g_nstrip < 2) { g_strip[g_nstrip++] = nv; break; }
        g_strip[2] = nv;
        if (g_strip_count & 1) submit_poly(g_strip, 3);
        else { V t[3] = {g_strip[1], g_strip[0], g_strip[2]}; submit_poly(t, 3); }
        g_strip[0] = g_strip[1];
        g_strip[1] = g_strip[2];
        break;
    case 3:                                         // quad strip: 0 1 3 2, then on in pairs
        if (g_nstrip < 3) { g_strip[g_nstrip++] = nv; break; }
        g_strip[3] = nv;
        { V q[4] = {g_strip[0], g_strip[1], g_strip[3], g_strip[2]}; submit_poly(q, 4); }
        g_strip[0] = g_strip[2];
        g_strip[1] = g_strip[3];
        g_nstrip = 2;
        break;
    }
}

// ---- the tests ----------------------------------------------------------------------------------
static void box_test(const uint32_t* p) {
    int16_t x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    int16_t w = (int16_t)(p[1] >> 16), h = (int16_t)p[2], d = (int16_t)(p[2] >> 16);
    update_clip();
    V c[8];
    for (int i = 0; i < 8; ++i) {
        int32_t px = x + ((i & 1) ? w : 0), py = y + ((i & 2) ? h : 0), pz = z + ((i & 4) ? d : 0);
        for (int k = 0; k < 4; ++k)
            c[i].p[k] = (int32_t)(((int64_t)px * mtx_clip[k] + (int64_t)py * mtx_clip[4 + k] + (int64_t)pz * mtx_clip[8 + k] +
                                   (int64_t)0x1000 * mtx_clip[12 + k]) >> 12);
        std::memset(c[i].c, 0, sizeof c[i].c);
        std::memset(c[i].t, 0, sizeof c[i].t);
    }
    static const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    bool in = false;
    for (int f = 0; f < 6 && !in; ++f) {
        V v[10];
        for (int k = 0; k < 4; ++k) v[k] = c[faces[f][k]];
        int n = clip_axis<2>(v, 4);
        if (n) n = clip_axis<0>(v, n);
        if (n) n = clip_axis<1>(v, n);
        in = n > 0;
    }
    g_stat = (g_stat & ~2u) | (in ? 2u : 0);
}

static int32_t g_pos_result[4];
static int16_t g_vec_result[3];

// ---- the commands -------------------------------------------------------------------------------
static void exec(uint32_t cmd, const uint32_t* p) {
    switch (cmd) {
    case 0x00: break;
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11:                                      // MTX_PUSH
        if (mtx_mode == 0) { if (sp_proj) g_stat |= 0x8000; std::memcpy(stk_proj, mtx_proj, 64); sp_proj = 1; }
        else if (mtx_mode == 3) { if (sp_tex) g_stat |= 0x8000; std::memcpy(stk_tex, mtx_tex, 64); sp_tex = 1; }
        else {
            if (sp_pos > 30) g_stat |= 0x8000;
            std::memcpy(stk_pos[sp_pos & 31], mtx_pos, 64);
            std::memcpy(stk_vec[sp_pos & 31], mtx_vec, 64);
            sp_pos = (sp_pos + 1) & 0x3F;
        }
        break;
    case 0x12:                                      // MTX_POP
        if (mtx_mode == 0) { if (!sp_proj) g_stat |= 0x8000; sp_proj = 0; std::memcpy(mtx_proj, stk_proj, 64); clip_dirty = true; }
        else if (mtx_mode == 3) { if (!sp_tex) g_stat |= 0x8000; sp_tex = 0; std::memcpy(mtx_tex, stk_tex, 64); }
        else {
            int32_t off = (int32_t)(p[0] << 26) >> 26;
            sp_pos = (uint32_t)(sp_pos - off) & 0x3F;
            if (sp_pos > 30) g_stat |= 0x8000;
            std::memcpy(mtx_pos, stk_pos[sp_pos & 31], 64);
            std::memcpy(mtx_vec, stk_vec[sp_pos & 31], 64);
            clip_dirty = true;
        }
        break;
    case 0x13: {                                    // MTX_STORE
        if (mtx_mode == 0) std::memcpy(stk_proj, mtx_proj, 64);
        else if (mtx_mode == 3) std::memcpy(stk_tex, mtx_tex, 64);
        else {
            uint32_t a = p[0] & 31;
            if (a == 31) g_stat |= 0x8000;
            std::memcpy(stk_pos[a], mtx_pos, 64);
            std::memcpy(stk_vec[a], mtx_vec, 64);
        }
        break;
    }
    case 0x14: {                                    // MTX_RESTORE
        if (mtx_mode == 0) { std::memcpy(mtx_proj, stk_proj, 64); clip_dirty = true; }
        else if (mtx_mode == 3) std::memcpy(mtx_tex, stk_tex, 64);
        else {
            uint32_t a = p[0] & 31;
            if (a == 31) g_stat |= 0x8000;
            std::memcpy(mtx_pos, stk_pos[a], 64);
            std::memcpy(mtx_vec, stk_vec[a], 64);
            clip_dirty = true;
        }
        break;
    }
    case 0x15: on_current([](int32_t* m) { identity(m); }, true); break;
    case 0x16: on_current([&](int32_t* m) { for (int i = 0; i < 16; ++i) m[i] = (int32_t)p[i]; }, true); break;
    case 0x17: on_current([&](int32_t* m) {
                   for (int r = 0; r < 4; ++r) {
                       for (int c = 0; c < 3; ++c) m[r * 4 + c] = (int32_t)p[r * 3 + c];
                       m[r * 4 + 3] = r == 3 ? 0x1000 : 0;
                   }
               }, true);
        break;
    case 0x18: on_current([&](int32_t* m) { mult44(m, (const int32_t*)p); }, true); break;
    case 0x19: on_current([&](int32_t* m) { mult43(m, (const int32_t*)p); }, true); break;
    case 0x1A: on_current([&](int32_t* m) { mult33(m, (const int32_t*)p); }, true); break;
    case 0x1B: on_current([&](int32_t* m) { scale(m, (const int32_t*)p); }, false); break;
    case 0x1C: on_current([&](int32_t* m) { translate(m, (const int32_t*)p); }, true); break;
    case 0x20:                                      // COLOR
        vcolor[0] = p[0] & 31; vcolor[1] = (p[0] >> 5) & 31; vcolor[2] = (p[0] >> 10) & 31;
        break;
    case 0x21:                                      // NORMAL
        for (int i = 0; i < 3; ++i) normal[i] = (int16_t)((int32_t)(p[0] >> (10 * i) << 22) >> 22);
        if (tex_param >> 30 == 2)
            for (int i = 0; i < 2; ++i)
                tc[i] = (int16_t)(((int64_t)(normal[0] << 3) * mtx_tex[i] + (int64_t)(normal[1] << 3) * mtx_tex[4 + i] +
                                   (int64_t)(normal[2] << 3) * mtx_tex[8 + i] + ((int64_t)raw_tc[i] << 24)) >> 24);
        lighting();
        break;
    case 0x22:                                      // TEXCOORD
        raw_tc[0] = (int16_t)p[0]; raw_tc[1] = (int16_t)(p[0] >> 16);
        if (tex_param >> 30 == 1) {
            for (int i = 0; i < 2; ++i)
                tc[i] = (int16_t)(((int64_t)raw_tc[0] * mtx_tex[i] + (int64_t)raw_tc[1] * mtx_tex[4 + i] +
                                   (int64_t)mtx_tex[8 + i] + (int64_t)mtx_tex[12 + i]) >> 12);
        } else {
            tc[0] = raw_tc[0]; tc[1] = raw_tc[1];
        }
        break;
    case 0x23: vtx[0] = (int16_t)p[0]; vtx[1] = (int16_t)(p[0] >> 16); vtx[2] = (int16_t)p[1]; add_vertex(); break;
    case 0x24:
        for (int i = 0; i < 3; ++i) vtx[i] = (int16_t)(((p[0] >> (10 * i)) & 0x3FF) << 6);
        add_vertex();
        break;
    case 0x25: vtx[0] = (int16_t)p[0]; vtx[1] = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x26: vtx[0] = (int16_t)p[0]; vtx[2] = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x27: vtx[1] = (int16_t)p[0]; vtx[2] = (int16_t)(p[0] >> 16); add_vertex(); break;
    case 0x28:
        for (int i = 0; i < 3; ++i) vtx[i] = (int16_t)(vtx[i] + ((int32_t)(p[0] >> (10 * i) << 22) >> 22));
        add_vertex();
        break;
    case 0x29: attr_pending = p[0]; break;
    case 0x2A: tex_param = p[0]; break;
    case 0x2B: pltt_base = p[0] & 0x1FFF; break;
    case 0x30:                                      // DIF_AMB
        diffuse[0] = p[0] & 31; diffuse[1] = (p[0] >> 5) & 31; diffuse[2] = (p[0] >> 10) & 31;
        ambient[0] = (p[0] >> 16) & 31; ambient[1] = (p[0] >> 21) & 31; ambient[2] = (p[0] >> 26) & 31;
        if (p[0] & 0x8000) std::memcpy(vcolor, diffuse, 3);
        break;
    case 0x31:                                      // SPE_EMI
        specular[0] = p[0] & 31; specular[1] = (p[0] >> 5) & 31; specular[2] = (p[0] >> 10) & 31;
        emission[0] = (p[0] >> 16) & 31; emission[1] = (p[0] >> 21) & 31; emission[2] = (p[0] >> 26) & 31;
        shine_table_on = p[0] & 0x8000;
        break;
    case 0x32: {                                    // LIGHT_VECTOR
        int l = p[0] >> 30;
        int32_t d[3];
        for (int i = 0; i < 3; ++i) d[i] = (int32_t)(p[0] >> (10 * i) << 22) >> 22;
        for (int i = 0; i < 3; ++i)
            light_vec[l][i] = (int32_t)(((int64_t)d[0] * mtx_vec[i] + (int64_t)d[1] * mtx_vec[4 + i] +
                                         (int64_t)d[2] * mtx_vec[8 + i]) >> 12);
        break;
    }
    case 0x33: {
        int l = p[0] >> 30;
        light_col[l][0] = p[0] & 31; light_col[l][1] = (p[0] >> 5) & 31; light_col[l][2] = (p[0] >> 10) & 31;
        break;
    }
    case 0x34:
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 4; ++k) shine_table[i * 4 + k] = (uint8_t)(p[i] >> (8 * k));
        break;
    case 0x40:                                      // BEGIN_VTXS
        prim = (int)(p[0] & 3);
        attr = attr_pending;
        g_nstrip = 0;
        g_strip_count = 0;
        break;
    case 0x41: break;                               // END_VTXS: nothing on the DS either
    case 0x50:                                      // SWAP_BUFFERS: at the VBlank
        g_swap_pending = true;
        g_swap_param = p[0];
        break;
    case 0x60: {                                    // VIEWPORT (y counted from the bottom)
        uint8_t x1 = (uint8_t)p[0], y1 = (uint8_t)(p[0] >> 8), x2 = (uint8_t)(p[0] >> 16), y2 = (uint8_t)(p[0] >> 24);
        viewport[0] = x1;
        viewport[1] = (uint8_t)(191 - y2);
        viewport[2] = x2;
        viewport[3] = (uint8_t)(191 - y1);
        break;
    }
    case 0x70: box_test(p); break;
    case 0x71:                                      // POS_TEST
        vtx[0] = (int16_t)p[0]; vtx[1] = (int16_t)(p[0] >> 16); vtx[2] = (int16_t)p[1];
        update_clip();
        for (int i = 0; i < 4; ++i)
            g_pos_result[i] = (int32_t)(((int64_t)vtx[0] * mtx_clip[i] + (int64_t)vtx[1] * mtx_clip[4 + i] +
                                         (int64_t)vtx[2] * mtx_clip[8 + i] + (int64_t)0x1000 * mtx_clip[12 + i]) >> 12);
        break;
    case 0x72: {                                    // VEC_TEST
        int32_t n[3];
        for (int i = 0; i < 3; ++i) n[i] = (int32_t)(p[0] >> (10 * i) << 22) >> 22;
        for (int i = 0; i < 3; ++i) {
            int32_t r = (int32_t)(((int64_t)n[0] * mtx_vec[i] + (int64_t)n[1] * mtx_vec[4 + i] + (int64_t)n[2] * mtx_vec[8 + i]) >> 9);
            g_vec_result[i] = (int16_t)((r & 0x1000) ? (r | 0xF000) : (r & 0x0FFF));
        }
        break;
    }
    default:
        nds_once("gx cmd", cmd, "3D: command %02X not known", cmd);
        break;
    }
}

// A whole command: now, or behind a swap.
static void command(uint32_t cmd, const uint32_t* p) {
    if (g_swap_pending) {
        Queued q;
        q.cmd = (uint8_t)cmd;
        q.n = kParams[cmd & 0xFF];
        std::memcpy(q.p, p, q.n * 4);
        g_queue.push_back(q);
        return;
    }
    exec(cmd, p);
}

static void fifo_word(uint32_t v) {
    if (g_cur >= 0) {                               // a parameter of the command being gathered
        g_params[g_got++] = v;
        if (g_got < g_need) return;
        command((uint32_t)g_cur, g_params);
        g_cur = -1;
    } else {
        g_cmds = v;
        g_ncmds = 4;
    }
    // the packed commands that need no parameters run now; the next one with parameters waits for them
    while (g_ncmds > 0) {
        uint32_t c = g_cmds & 0xFF;
        g_cmds >>= 8;
        --g_ncmds;
        if (!known(c)) nds_once("gx cmd", c, "3D: packed command %02X not known", c);
        int need = c < 0x80 ? kParams[c] : 0;
        if (need == 0) {
            if (c) command(c, g_params);
            if (!g_cmds) g_ncmds = 0;               // only NOPs left in the word
            continue;
        }
        g_cur = (int)c; g_need = need; g_got = 0;
        return;
    }
}

static int g_port_got[0x80];
static uint32_t g_port_params[0x80][32];

static void port_write(uint32_t cmd, uint32_t v) {
    int need = kParams[cmd];
    if (need == 0) { command(cmd, g_port_params[cmd]); return; }
    g_port_params[cmd][g_port_got[cmd]++] = v;
    if (g_port_got[cmd] < need) return;
    g_port_got[cmd] = 0;
    command(cmd, g_port_params[cmd]);
}

// ---- the registers ------------------------------------------------------------------------------
static uint32_t gxstat() {
    uint32_t v = g_stat & 0xC0008002u;
    v |= (sp_pos & 31) << 8 | (sp_proj & 1) << 13;
    uint32_t n = (uint32_t)std::min<size_t>(g_queue.size(), 256);
    v |= n << 16;
    if (n < 128) v |= 1u << 25;
    if (n == 0) v |= 1u << 26;
    if (g_swap_pending) v |= 1u << 27;
    return v;
}

static void fifo_irq() {
    uint32_t m = g_stat >> 30, n = (uint32_t)g_queue.size();
    if ((m == 1 && n < 128) || (m == 2 && n == 0)) nds_irq_raise(9, 21);
}

void gx_write(uint32_t a, uint32_t v, int size) {
    uint32_t off = a - 0x04000320u;
    if (a >= 0x04000400u && a < 0x04000440u) {
        if (size != 4) nds_once("gx", 2, "3D: a %d-byte write to GXFIFO", size);
        fifo_word(v);
        return;
    }
    if (a >= 0x04000440u && a < 0x04000600u) {
        if (size != 4) nds_once("gx", 3, "3D: a %d-byte write to a command port", size);
        port_write((a - 0x04000400u) >> 2, v);
        return;
    }
    if (a == 0x04000600u || a == 0x04000602u) {     // GXSTAT: the FIFO interrupt's mode, the error bit acknowledged
        uint32_t w = (a == 0x04000602u && size == 2) ? v << 16 : v;
        if (size == 4 || a == 0x04000602u) g_stat = (g_stat & ~0xC0000000u) | (w & 0xC0000000u);
        if ((size == 4 || a == 0x04000600u) && (w & 0x8000)) { g_stat &= ~0x8000u; sp_proj = 0; sp_tex = 0; }
        fifo_irq();
        return;
    }
    if (off + size <= sizeof g_regs) std::memcpy(&g_regs[off], &v, size);
}

uint32_t gx_read(uint32_t a, int size) {
    (void)size;
    uint32_t off = a & ~3u;
    if (off == 0x04000600u) return gxstat();
    if (off == 0x04000604u) return (uint32_t)g_build.polys.size() | g_build.nverts_ds << 16;
    if (off >= 0x04000620u && off < 0x04000630u) return (uint32_t)g_pos_result[(off - 0x04000620u) >> 2];
    if (off >= 0x04000630u && off < 0x04000636u) {
        uint32_t i = (off - 0x04000630u) >> 1;
        return (uint16_t)g_vec_result[i] | (i < 2 ? (uint32_t)(uint16_t)g_vec_result[i + 1] << 16 : 0);
    }
    if (off >= 0x04000640u && off < 0x04000680u) { update_clip(); return (uint32_t)mtx_clip[(off - 0x04000640u) >> 2]; }
    if (off >= 0x04000680u && off < 0x040006A4u) {
        uint32_t i = (off - 0x04000680u) >> 2;
        return (uint32_t)mtx_vec[(i / 3) * 4 + i % 3];
    }
    uint32_t o = off - 0x04000320u, v = 0;
    if (o + 4 <= sizeof g_regs) std::memcpy(&v, &g_regs[o], 4);
    return v;
}

// ---- the swap, and the rendering engine's job -----------------------------------------------------
void gx_vblank() {
    if (!g_swap_pending) return;
    g_swap_pending = false;
    std::swap(g_render, g_build);
    g_build.verts.clear();
    g_build.polys.clear();
    g_build.nverts_ds = 0;
    g_render_manual = g_swap_param & 1;
    g_render_wbuf = g_swap_param & 2;
    ++g_swaps;
    while (!g_queue.empty() && !g_swap_pending) {
        Queued q = g_queue.front();
        g_queue.pop_front();
        exec(q.cmd, q.p);
    }
    fifo_irq();
}

void gx_line() { fifo_irq(); }

static uint16_t r16(uint32_t a) { uint16_t v; std::memcpy(&v, &g_regs[a - 0x04000320u], 2); return v; }

void gx_render_start() {
    if (!g_2d_on) return;                           // a headless run with no picture of the next frame
    Render3dJob& J = g_job;
    J.valid = true;
    J.frame = g_swaps;
    J.verts = g_render.verts;
    // the order of drawing: opaque polygons, then translucent ones; each sorted by y
    // unless SWAP_BUFFERS asked for the translucent ones in their own order
    J.polys.clear();
    for (const GxPoly& p : g_render.polys) if (!p.translucent) J.polys.push_back(p);
    size_t nopaque = J.polys.size();
    for (const GxPoly& p : g_render.polys) if (p.translucent) J.polys.push_back(p);
    auto by_y = [](const GxPoly& a, const GxPoly& b) { return a.sortkey < b.sortkey; };
    std::stable_sort(J.polys.begin(), J.polys.begin() + (long)nopaque, by_y);
    if (!g_render_manual) std::stable_sort(J.polys.begin() + (long)nopaque, J.polys.end(), by_y);
    J.wbuffer = g_render_wbuf;
    J.disp3dcnt = io_reg16(9, 0x04000060u);
    uint32_t cc;
    std::memcpy(&cc, &g_regs[0x350 - 0x320], 4);
    J.clear_color = cc;
    J.clear_depth = r16(0x04000354u) & 0x7FFF;
    J.clrimage_offset = r16(0x04000356u);
    J.alpha_ref = g_regs[0x340 - 0x320] & 31;
    for (int i = 0; i < 8; ++i) J.edge[i] = r16(0x04000330u + 2 * i);
    std::memcpy(&J.fog_color, &g_regs[0x358 - 0x320], 4);
    J.fog_offset = r16(0x0400035Cu);
    std::memcpy(J.fog_table, &g_regs[0x360 - 0x320], 32);
    for (int i = 0; i < 32; ++i) J.toon[i] = r16(0x04000380u + 2 * i);
    J.tex.resize(0x80000);
    J.texpal.resize(0x18000);
    vram_copy(VRAM_TEX, J.tex.data(), 0x80000);
    vram_copy(VRAM_TEXPAL, J.texpal.data(), 0x18000);
}

const Render3dJob& gx_job() { return g_job; }

void gx_reset() {
    std::memset(g_regs, 0, sizeof g_regs);
    identity(mtx_proj); identity(mtx_pos); identity(mtx_vec); identity(mtx_tex);
    clip_dirty = true;
    sp_proj = sp_tex = sp_pos = 0;
    mtx_mode = 0;
    g_stat = 0;
    viewport[0] = 0; viewport[1] = 0; viewport[2] = 255; viewport[3] = 191;
    g_build = Lists{};
    g_render = Lists{};
    g_job = Render3dJob{};
    g_swap_pending = false;
    g_queue.clear();
    g_cur = -1; g_ncmds = 0;
    std::memset(g_port_got, 0, sizeof g_port_got);
    prim = -1;
}
