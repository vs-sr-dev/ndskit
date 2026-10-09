// ndskit runtime — the rendering engine on the host's GPU (OpenGL 4.5).
//
// A frame's polygons (render3d.h) are drawn into a target of 256x192 times
// the scale: at 1 to compare with the DS, higher in the window. The screen
// positions are the exact ones the geometry engine computed (at scale 1 the
// DS's own, rounded as it rounds them); colours and texture coordinates are
// interpolated with perspective at each pixel's top left corner, as the DS
// does (texture coordinates kept in 1/16 texel); depth is z/w linearly
// across the screen, or w (SWAP_BUFFERS bit 1).
//
// Textures are not decoded on the CPU: the texture and texture-palette
// slots are uploaded whole each frame (608 KiB) and the fragment shader
// reads them as the DS's texture unit does: the seven formats, 4x4
// compressed included, repeat, flip and clamp, colour 0 transparent. The
// shader then works in the DS's integers: 6-bit colours, 5-bit alpha, the
// modulation, decal, toon and highlight modes, the alpha test.
//
// Order: the opaque polygons, then the translucent ones blended (when
// DISP3DCNT enables it), their depth written by POLYGON_ATTR bit 11; the
// rendering engine's sort is the geometry engine's (gx.cpp). Wireframe
// polygons (alpha 0) are drawn as lines. Fog follows, in a pass over the
// picture: each pixel's fog flag (POLYGON_ATTR bit 15; a translucent pixel
// keeps it only where the pixel under it had it) and its depth.
//
// Shadow polygons (mode 3) through the stencil: a mask (polygon ID 0) marks
// where it fails the depth test, the shadows then draw, blended, only there;
// the DS's rule that a shadow skips pixels of its own polygon ID is not
// followed yet.
//
// Not yet: the translucent polygon ID rule, the equal depth test, edge marking, anti-aliasing, the clear
// image; a translucent pixel over a clear pixel of alpha 0 is blended with
// the clear colour, where the DS writes it as it is.
//
// NDSKIT_POLYS=1 in the environment prints each frame's polygons as they
// are drawn (their attributes, screen positions, texture coordinates, w).
#include "gl.h"
#include "nds.h"
#include "render3d.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <vector>

namespace {

const char* kVS = R"(#version 450
layout(location = 0) in vec4 a_pos;     // screen x, y (pixels, y down), z/w depth (0-1), w (> 0)
layout(location = 1) in vec3 a_col;     // 0-31
layout(location = 2) in vec2 a_tc;      // texels
layout(location = 3) in float a_wraw;   // w as the DS keeps it (20.12, as an integer)
layout(location = 4) in uvec4 a_poly;   // POLYGON_ATTR, TEXIMAGE_PARAM, PLTT_BASE, flags
out vec3 v_col;
out vec2 v_tc;
out float v_wraw;
noperspective out float v_z;
flat out uvec4 v_poly;
void main() {
    float w = a_pos.w;
    gl_Position = vec4((a_pos.x / 128.0 - 1.0) * w, (1.0 - a_pos.y / 96.0) * w, 0.0, w);
    v_col = a_col;
    v_tc = a_tc;
    v_wraw = a_wraw;
    v_z = a_pos.z;
    v_poly = a_poly;
}
)";

const char* kFS = R"(#version 450
in vec3 v_col;
in vec2 v_tc;
in float v_wraw;
noperspective in float v_z;
flat in uvec4 v_poly;
layout(binding = 0) uniform usamplerBuffer u_tex;   // the texture slots, bytes
layout(binding = 1) uniform usamplerBuffer u_pal;   // the texture-palette slots, halfwords
uniform uint u_disp;                                // DISP3DCNT
uniform uint u_alpharef;
uniform uint u_wbuf;
uniform uint u_toon[32];
layout(location = 0) out vec4 o_col;
layout(location = 1) out vec4 o_attr;               // r: the fog flag

uint rd8(uint a) { return texelFetch(u_tex, int(a & 0x7FFFFu)).r; }
uint rd16(uint a) { return rd8(a) | rd8(a + 1u) << 8; }
uint rd32(uint a) { return rd16(a) | rd16(a + 2u) << 16; }
uint pal(uint i) { return texelFetch(u_pal, int(i % 0xC000u)).r; }
uint c6(uint c5) { return c5 == 0u ? 0u : c5 * 2u + 1u; }
uvec3 rgb6(uint c) { return uvec3(c6(c & 31u), c6(c >> 5 & 31u), c6(c >> 10 & 31u)); }

int wrap(int v, int size, bool rep, bool flip) {
    if (!rep) return clamp(v, 0, size - 1);
    if (flip) {
        int m = v & (2 * size - 1);
        return m >= size ? 2 * size - 1 - m : m;
    }
    return v & (size - 1);
}

// a texel: rgb 0-63, alpha 0-31
uvec4 texel(uint p, uint pb, ivec2 st) {
    int w = 8 << int(p >> 20 & 7u), h = 8 << int(p >> 23 & 7u);
    int s = wrap(st.x, w, (p & 0x10000u) != 0u, (p & 0x40000u) != 0u);
    int t = wrap(st.y, h, (p & 0x20000u) != 0u, (p & 0x80000u) != 0u);
    uint fmt = p >> 26 & 7u;
    uint addr = (p & 0xFFFFu) << 3;
    uint i = uint(t * w + s);
    bool tr0 = (p & 0x20000000u) != 0u;
    uint pbase = pb << 3;                              // in halfwords: PLTT_BASE x 16 bytes
    if (fmt == 1u) {                                   // A3I5
        uint b = rd8(addr + i);
        uint a = b >> 5;
        return uvec4(rgb6(pal(pbase + (b & 31u))), a * 4u + (a >> 1));
    }
    if (fmt == 2u) {                                   // 4 colours
        uint x = rd8(addr + i / 4u) >> (2u * (i & 3u)) & 3u;
        if (tr0 && x == 0u) return uvec4(0u);
        return uvec4(rgb6(pal((pb << 2) + x)), 31u);
    }
    if (fmt == 3u) {                                   // 16 colours
        uint x = rd8(addr + i / 2u) >> (4u * (i & 1u)) & 15u;
        if (tr0 && x == 0u) return uvec4(0u);
        return uvec4(rgb6(pal(pbase + x)), 31u);
    }
    if (fmt == 4u) {                                   // 256 colours
        uint x = rd8(addr + i);
        if (tr0 && x == 0u) return uvec4(0u);
        return uvec4(rgb6(pal(pbase + x)), 31u);
    }
    if (fmt == 5u) {                                   // 4x4 compressed: blocks in slot 0 or 2, their palette words in slot 1
        uint blk = addr + uint((t >> 2) * (w >> 2) + (s >> 2)) * 4u;
        uint x = rd32(blk) >> (uint(t & 3) * 8u + uint(s & 3) * 2u) & 3u;
        uint slot = blk >> 17;
        uint ia = 0x20000u + ((blk & 0x1FFFFu) >> 1) + (slot == 2u ? 0x10000u : 0u);
        uint pw = rd16(ia);
        uint base = pbase + (pw & 0x3FFFu) * 2u;
        uint mode = pw >> 14;
        uvec3 a = rgb6(pal(base)), b = rgb6(pal(base + 1u));
        if (x == 0u) return uvec4(a, 31u);
        if (x == 1u) return uvec4(b, 31u);
        if (mode == 0u) return x == 2u ? uvec4(rgb6(pal(base + 2u)), 31u) : uvec4(0u);
        if (mode == 1u) return x == 2u ? uvec4((a + b) / 2u, 31u) : uvec4(0u);
        if (mode == 2u) return uvec4(rgb6(pal(base + x)), 31u);
        return x == 2u ? uvec4((a * 5u + b * 3u) / 8u, 31u) : uvec4((a * 3u + b * 5u) / 8u, 31u);
    }
    if (fmt == 6u) {                                   // A5I3
        uint b = rd8(addr + i);
        return uvec4(rgb6(pal(pbase + (b & 7u))), b >> 3);
    }
    uint c = rd16(addr + i * 2u);                      // direct colour
    return uvec4(rgb6(c), (c & 0x8000u) != 0u ? 31u : 0u);
}

void main() {
    uint attr = v_poly.x, tp = v_poly.y;
    uint mode = attr >> 4 & 3u;
    uint pa = attr >> 16 & 31u;
    if (pa == 0u) pa = 31u;                            // wireframe: drawn as lines, opaque
    // the DS interpolates at a pixel's top left corner, GL at its centre (y up here)
    vec3 col = interpolateAtOffset(v_col, vec2(-0.5, 0.5));
    vec2 tc = interpolateAtOffset(v_tc, vec2(-0.5, 0.5));
    uvec3 vc = uvec3(clamp(floor(col * 2.0), 0.0, 63.0));
    uvec3 rgb;
    uint a;
    if (mode == 2u) {
        uint tc = u_toon[vc.r >> 1];
        if ((u_disp & 2u) != 0u) vc = uvec3(vc.r);    // highlight: the red component as grey, the table added after
        else vc = rgb6(tc);
    }
    bool textured = (u_disp & 1u) != 0u && (tp >> 26 & 7u) != 0u;
    if (textured) {
        uvec4 t = texel(tp, v_poly.z, ivec2(floor(tc * 16.0 + 1.0 / 64.0)) >> 4);   // the DS keeps 1/16 texel
        if (mode == 1u) {
            if (t.a == 0u) rgb = vc;
            else if (t.a == 31u) rgb = t.rgb;
            else rgb = (t.rgb * t.a + vc * (31u - t.a)) >> 5;
            a = pa;
        } else {
            rgb = ((t.rgb + 1u) * (vc + 1u) - 1u) >> 6;
            a = ((t.a + 1u) * (pa + 1u) - 1u) >> 5;
        }
    } else {
        rgb = vc;
        a = pa;
    }
    if (mode == 2u && (u_disp & 2u) != 0u) rgb = min(rgb + rgb6(u_toon[vc.r >> 1]), uvec3(63u));
    if (a == 0u) discard;
    if ((u_disp & 4u) != 0u && a <= u_alpharef) discard;
    o_col = vec4(vec3(rgb) / 63.0, float(a) / 31.0);
    o_attr = vec4((attr & 0x8000u) != 0u ? 1.0 : 0.0, 0.0, 0.0, 1.0);
    gl_FragDepth = u_wbuf != 0u ? clamp(v_wraw / 16777215.0, 0.0, 1.0) : v_z;
}
)";

const char* kFullVS = R"(#version 450
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Fog, as the DS applies it after drawing: by the depth of each pixel whose
// fog flag is set, a density from FOG_TABLE (32 steps of 0x400 >> FOG_SHIFT
// from FOG_OFFSET, interpolated), the fog colour blended in by it (or only
// its alpha, DISP3DCNT bit 6).
const char* kFogFS = R"(#version 450
layout(binding = 0) uniform sampler2D u_col;
layout(binding = 1) uniform sampler2D u_attr;
layout(binding = 2) uniform sampler2D u_depth;
uniform uint u_disp;
uniform uint u_wbuf;
uniform uint u_fogcol;
uniform uint u_fogoff;
uniform uint u_table[32];
layout(location = 0) out vec4 o_col;
uint c6(uint c5) { return c5 == 0u ? 0u : c5 * 2u + 1u; }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(u_col, p, 0);
    o_col = c;
    if ((u_disp & 0x80u) == 0u || texelFetch(u_attr, p, 0).r < 0.5) return;
    float d = texelFetch(u_depth, p, 0).r;
    uint z = u_wbuf != 0u ? uint(clamp(d * 16777215.0, 0.0, 16777215.0))
                          : uint(clamp(((d * 2.0 - 1.0) * 16384.0 + 16383.0) * 512.0, 0.0, 16777215.0));
    uint off = u_fogoff * 0x200u, id = 0u, frac = 0u;
    if (z >= off) {
        uint q = ((z - off) >> 2) << (u_disp >> 8 & 15u);
        id = q >> 17;
        frac = q & 0x1FFFFu;
        if (id >= 32u) { id = 32u; frac = 0u; }
    }
    // the table as the DS reads it: its first entry before the first step, its last after the last
    uint d0 = u_table[id == 0u ? 0u : min(id - 1u, 31u)], d1 = u_table[min(id, 31u)];
    uint den = (d0 * (0x20000u - frac) + d1 * frac) >> 17;
    if (den >= 127u) den = 128u;
    uvec3 rgb = uvec3(round(c.rgb * 63.0));
    uint a = uint(round(c.a * 31.0));
    uvec3 frgb = uvec3(c6(u_fogcol & 31u), c6(u_fogcol >> 5 & 31u), c6(u_fogcol >> 10 & 31u));
    uint fa = u_fogcol >> 16 & 31u;
    if ((u_disp & 0x40u) == 0u) rgb = (frgb * den + rgb * (128u - den)) >> 7;
    a = (fa * den + a * (128u - den)) >> 7;
    o_col = vec4(vec3(rgb) / 63.0, float(a) / 31.0);
}
)";

struct Vtx {
    float pos[4];
    float col[3];
    float tc[2];
    float wraw;
    uint32_t poly[4];
};

GLuint prog, vao, vbo, texbuf, palbuf, textex, paltex, fog_prog, empty_vao;
size_t vbo_cap;
GLint loc_disp, loc_alpharef, loc_wbuf, loc_toon;
GLint fl_disp, fl_wbuf, fl_col, fl_off, fl_table;

}  // namespace

bool render_init() {
    prog = gl_program(kVS, kFS);
    if (!prog) return false;
    loc_disp = glGetUniformLocation(prog, "u_disp");
    loc_alpharef = glGetUniformLocation(prog, "u_alpharef");
    loc_wbuf = glGetUniformLocation(prog, "u_wbuf");
    loc_toon = glGetUniformLocation(prog, "u_toon");
    fog_prog = gl_program(kFullVS, kFogFS);
    if (!fog_prog) return false;
    fl_disp = glGetUniformLocation(fog_prog, "u_disp");
    fl_wbuf = glGetUniformLocation(fog_prog, "u_wbuf");
    fl_col = glGetUniformLocation(fog_prog, "u_fogcol");
    fl_off = glGetUniformLocation(fog_prog, "u_fogoff");
    fl_table = glGetUniformLocation(fog_prog, "u_table");
    glCreateVertexArrays(1, &empty_vao);
    glCreateVertexArrays(1, &vao);
    glCreateBuffers(1, &vbo);
    vbo_cap = 1 << 20;
    glNamedBufferData(vbo, (GLsizeiptr)vbo_cap, nullptr, GL_STREAM_DRAW);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, sizeof(Vtx));
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 4, GL_FLOAT, GL_FALSE, offsetof(Vtx, pos));
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vtx, col));
    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vtx, tc));
    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 3, 1, GL_FLOAT, GL_FALSE, offsetof(Vtx, wraw));
    glEnableVertexArrayAttrib(vao, 4);
    glVertexArrayAttribIFormat(vao, 4, 4, GL_UNSIGNED_INT, offsetof(Vtx, poly));
    for (int i = 0; i < 5; ++i) glVertexArrayAttribBinding(vao, i, 0);
    glCreateBuffers(1, &texbuf);
    glNamedBufferData(texbuf, 0x80000, nullptr, GL_STREAM_DRAW);
    glCreateBuffers(1, &palbuf);
    glNamedBufferData(palbuf, 0x18000, nullptr, GL_STREAM_DRAW);
    glCreateTextures(GL_TEXTURE_BUFFER, 1, &textex);
    glTextureBuffer(textex, GL_R8UI, texbuf);
    glCreateTextures(GL_TEXTURE_BUFFER, 1, &paltex);
    glTextureBuffer(paltex, GL_R16UI, palbuf);
    return true;
}

void render_target(GlTarget& t, int scale) {
    if (t.fbo && t.scale == scale) return;
    if (t.fbo) {
        glDeleteFramebuffers(1, &t.fbo);
        glDeleteFramebuffers(1, &t.fbo2);
        GLuint tx[4] = {t.color, t.attr, t.depth, t.out};
        glDeleteTextures(4, tx);
    }
    t.scale = scale;
    auto tex = [&](GLuint& name, GLenum fmt) {
        glCreateTextures(GL_TEXTURE_2D, 1, &name);
        glTextureStorage2D(name, 1, fmt, 256 * scale, 192 * scale);
        glTextureParameteri(name, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(name, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    };
    tex(t.color, GL_RGBA8);
    tex(t.attr, GL_RGBA8);
    tex(t.depth, GL_DEPTH32F_STENCIL8);
    tex(t.out, GL_RGBA8);
    glCreateFramebuffers(1, &t.fbo);
    glNamedFramebufferTexture(t.fbo, GL_COLOR_ATTACHMENT0, t.color, 0);
    glNamedFramebufferTexture(t.fbo, GL_COLOR_ATTACHMENT1, t.attr, 0);
    glNamedFramebufferTexture(t.fbo, GL_DEPTH_STENCIL_ATTACHMENT, t.depth, 0);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(t.fbo, 2, bufs);
    glCreateFramebuffers(1, &t.fbo2);
    glNamedFramebufferTexture(t.fbo2, GL_COLOR_ATTACHMENT0, t.out, 0);
    if (glCheckNamedFramebufferStatus(t.fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckNamedFramebufferStatus(t.fbo2, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        nds_note("render: the 3D target at scale %d is not complete", scale);
}

static float zdepth(int32_t z, int32_t w) { return w ? (float)((double)z / w * 0.5 + 0.5) : 0.5f; }

static void draw_polys(const Render3dJob& J, GlTarget& T);

GLuint render_draw(const Render3dJob& J, GlTarget& T) {
    glBindFramebuffer(GL_FRAMEBUFFER, T.fbo);
    glViewport(0, 0, 256 * T.scale, 192 * T.scale);
    uint32_t cc = J.clear_color;
    auto c5 = [](uint32_t c) { return c == 0 ? 0.0f : (float)(c * 2 + 1) / 63.0f; };
    float col[4] = {c5(cc & 31), c5(cc >> 5 & 31), c5(cc >> 10 & 31), (float)(cc >> 16 & 31) / 31.0f};
    float fog[4] = {(cc & 0x8000) ? 1.0f : 0.0f, 0, 0, 1};
    double z24 = J.clear_depth * 512.0 + 511.0;
    float depth = (float)(J.wbuffer ? z24 / 16777215.0 : ((z24 / 512.0 - 0x3FFF) / 16384.0) * 0.5 + 0.5);
    glDepthMask(GL_TRUE);
    glClearNamedFramebufferfv(T.fbo, GL_COLOR, 0, col);
    glClearNamedFramebufferfv(T.fbo, GL_COLOR, 1, fog);
    glClearNamedFramebufferfv(T.fbo, GL_DEPTH, 0, &depth);
    if (J.valid && !J.polys.empty()) draw_polys(J, T);
    if (J.disp3dcnt & 0x20) nds_once("render", 1, "3D: edge marking asked for (not drawn)");
    if (!(J.disp3dcnt & 0x80)) return T.color;

    glBindFramebuffer(GL_FRAMEBUFFER, T.fbo2);
    glUseProgram(fog_prog);
    glProgramUniform1ui(fog_prog, fl_disp, J.disp3dcnt);
    glProgramUniform1ui(fog_prog, fl_wbuf, J.wbuffer ? 1u : 0u);
    glProgramUniform1ui(fog_prog, fl_col, J.fog_color);
    glProgramUniform1ui(fog_prog, fl_off, J.fog_offset & 0x7FFF);
    uint32_t table[32];
    for (int i = 0; i < 32; ++i) table[i] = J.fog_table[i] & 0x7F;
    glProgramUniform1uiv(fog_prog, fl_table, 32, table);
    glBindTextureUnit(0, T.color);
    glBindTextureUnit(1, T.attr);
    glBindTextureUnit(2, T.depth);
    glBindVertexArray(empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    return T.out;
}

static void draw_polys(const Render3dJob& J, GlTarget& T) {
    if (getenv("NDSKIT_POLYS")) {
        for (const GxPoly& p : J.polys) {
            std::fprintf(stderr, "poly attr %08X tex %08X pal %04X:", p.attr, p.tex, p.pal);
            for (uint32_t i = 0; i < p.n; ++i) {
                const GxVertex& g = J.verts[p.first + i];
                std::fprintf(stderr, " (%d,%d %.3f,%.3f st %.3f,%.3f w %d)", g.ix, g.iy, g.sx, g.sy, g.s, g.t, g.w);
            }
            std::fprintf(stderr, "\n");
        }
    }
    // the vertices, in drawing order; runs of the same state are one draw each
    static std::vector<Vtx> vs;
    // kind: 0 opaque, 1 translucent, 2 translucent + depth, 3 lines, 4 shadow mask (ID 0),
    // 5 shadow (ID 1-63), 6 shadow + depth
    struct Run { GLint first; GLsizei n; int kind; };
    static std::vector<Run> runs;
    vs.clear();
    runs.clear();
    bool snap = T.scale == 1;
    for (const GxPoly& p : J.polys) {
        uint32_t mode = p.attr >> 4 & 3, alpha = p.attr >> 16 & 31;
        if (p.attr & 0x4000) nds_once("render", 3, "3D: the equal depth test (drawn as less)");
        int kind = alpha == 0 ? 3 : !p.translucent ? 0 : (p.attr & 0x800) ? 2 : 1;
        if (mode == 3 && alpha) kind = (p.attr >> 24 & 63) == 0 ? 4 : (p.attr & 0x800) ? 6 : 5;
        GLint first = (GLint)vs.size();
        auto put = [&](const GxVertex& g) {
            Vtx v;
            v.pos[0] = snap ? (float)g.ix : g.sx;
            v.pos[1] = snap ? (float)g.iy : g.sy;
            v.pos[2] = zdepth(g.z, g.w);
            v.pos[3] = std::max(g.w, 1) / 4096.0f;
            v.col[0] = g.r; v.col[1] = g.g; v.col[2] = g.b;
            v.tc[0] = g.s; v.tc[1] = g.t;
            v.wraw = (float)g.w;
            v.poly[0] = p.attr; v.poly[1] = p.tex; v.poly[2] = p.pal; v.poly[3] = 0;
            vs.push_back(v);
        };
        const GxVertex* g = &J.verts[p.first];
        if (kind == 3) {
            for (uint32_t i = 0; i < p.n; ++i) { put(g[i]); put(g[(i + 1) % p.n]); }
        } else {
            for (uint32_t i = 1; i + 1 < p.n; ++i) { put(g[0]); put(g[i]); put(g[i + 1]); }
        }
        GLsizei n = (GLsizei)vs.size() - first;
        if (!runs.empty() && runs.back().kind == kind) runs.back().n += n;
        else runs.push_back({first, n, kind});
    }
    size_t bytes = vs.size() * sizeof(Vtx);
    if (bytes > vbo_cap) {
        while (vbo_cap < bytes) vbo_cap *= 2;
        glNamedBufferData(vbo, (GLsizeiptr)vbo_cap, nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(vbo, 0, (GLsizeiptr)bytes, vs.data());
    glNamedBufferSubData(texbuf, 0, 0x80000, J.tex.data());
    glNamedBufferSubData(palbuf, 0, 0x18000, J.texpal.data());

    glUseProgram(prog);
    glProgramUniform1ui(prog, loc_disp, J.disp3dcnt);
    glProgramUniform1ui(prog, loc_alpharef, J.alpha_ref);
    glProgramUniform1ui(prog, loc_wbuf, J.wbuffer ? 1u : 0u);
    uint32_t toon[32];
    for (int i = 0; i < 32; ++i) toon[i] = J.toon[i];
    glProgramUniform1uiv(prog, loc_toon, 32, toon);
    glBindVertexArray(vao);
    glBindTextureUnit(0, textex);
    glBindTextureUnit(1, paltex);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    const GLint zero = 0;
    glClearNamedFramebufferiv(T.fbo, GL_STENCIL, 0, &zero);
    int last = -1;
    for (const Run& r : runs) {
        // shadows: a mask (polygon ID 0) marks the stencil where it fails the depth test,
        // drawing nothing; the shadow polygons then draw only where it is marked; a mask
        // after shadows starts again from a clear stencil
        if (r.kind == 4) {
            if (last == 5 || last == 6) glClearNamedFramebufferiv(T.fbo, GL_STENCIL, 0, &zero);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);      // marked where the depth test fails
            glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_FALSE);
            glDrawArrays(GL_TRIANGLES, r.first, r.n);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_STENCIL_TEST);
            last = 4;
            continue;
        }
        if (r.kind == 5 || r.kind == 6) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }
        last = r.kind;
        bool blend = (r.kind == 1 || r.kind == 2 || r.kind == 5 || r.kind == 6) && (J.disp3dcnt & 8);
        if (blend) {
            // the colour blended, its alpha the greater; the fog flag kept only where both have it
            glEnablei(GL_BLEND, 0);
            glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_MAX);
            glEnablei(GL_BLEND, 1);
            glBlendFuncSeparatei(1, GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            glBlendEquationSeparatei(1, GL_MIN, GL_MIN);
        } else {
            glDisablei(GL_BLEND, 0);
            glDisablei(GL_BLEND, 1);
        }
        glDepthMask(r.kind == 1 || r.kind == 5 ? GL_FALSE : GL_TRUE);
        glDrawArrays(r.kind == 3 ? GL_LINES : GL_TRIANGLES, r.first, r.n);
        if (r.kind == 5 || r.kind == 6) glDisable(GL_STENCIL_TEST);
    }
    glDisablei(GL_BLEND, 0);
    glDisablei(GL_BLEND, 1);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glUseProgram(0);
}
