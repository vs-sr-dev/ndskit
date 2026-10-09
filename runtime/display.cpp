// ndskit runtime — the screens: the window, the compositor, the host's
// input, pictures.
//
// The window shows the two screens one above the other (the upper one is
// engine A's when POWCNT1 bit 15 says so), as large as it allows at the
// DS's shape. Each frame the 2D engines' words (nds.h) go up as a texture,
// the 3D picture is drawn at 256x192 times the scale (render_gl.cpp), and a
// shader composes them per pixel of the window: the 2D layers at the DS's
// resolution, the 3D at its own. Headless pictures do the same at the DS's
// size, composed on the CPU (gpu2d_compose) from the 3D read back.
//
// Input: the keys as the oracle's melonDS has them (X A, Z B, S X, A Y, Q L,
// W R, Enter START, Backspace SELECT, the arrows), the left mouse button on
// the lower screen as the stylus; F12 a picture into the run's directory.
//
// The pace: the DS's 59.83 frames a second by the host's clock, each frame
// presented with the vertical sync (again at each refresh until the next is
// due: twice on a display at 120 Hz). A run with an input script ignores the
// keyboard and the mouse.
#include "display.h"
#include "gl.h"
#include "render3d.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define NDSKIT_GL_DEFINE(T, n) T n = nullptr;
NDSKIT_GL_FUNCS(NDSKIT_GL_DEFINE)
#undef NDSKIT_GL_DEFINE

void spi_set_pen(bool down, int x, int y);

namespace {

SDL_Window* win;
SDL_GLContext ctx;
bool visible;
GlTarget t_win, t_one;                                  // the 3D at the window's scale, and at 1
GLuint comp_prog, words_tex, empty_vao;
GLint loc_atop, loc_view;
bool vsync;
double refresh_ns;
uint64_t next_ns;
std::string capture;                                    // a picture of the window, taken at the next present
const double kFrameNs = 1e9 * 263 * 355 * 6 / 33513982.0;   // 16.7154 ms
uint64_t shots_taken;
std::vector<uint32_t> words(256 * 384 * 2);

const char* kCompVS = R"(#version 450
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// gpu2d_compose, per pixel of the window
const char* kCompFS = R"(#version 450
layout(binding = 0) uniform usampler2D u_words;   // 256x384: the upper screen's words, then the lower's
layout(binding = 1) uniform sampler2D u_3d;       // engine A's 3D, rows bottom first
uniform vec4 u_view;                              // the screens' rectangle in the window: x, y (top), w, h
uniform int u_atop;                               // engine A on the upper screen
layout(location = 0) out vec4 o_col;

uint blend(uint a, uint b, uint eva, uint evb) {
    uint o = 0u;
    for (int s = 0; s < 18; s += 6) o |= min(((a >> s & 63u) * eva + (b >> s & 63u) * evb + 8u) >> 4, 63u) << s;
    return o;
}
uint brighter(uint a, uint evy) {
    uint o = 0u;
    for (int s = 0; s < 18; s += 6) { uint c = a >> s & 63u; o |= (c + ((63u - c) * evy >> 4)) << s; }
    return o;
}
uint darker(uint a, uint evy) {
    uint o = 0u;
    for (int s = 0; s < 18; s += 6) { uint c = a >> s & 63u; o |= (c - ((c * evy + 7u) >> 4)) << s; }
    return o;
}

void main() {
    vec2 win = vec2(gl_FragCoord.x - u_view.x, u_view.w - (gl_FragCoord.y - u_view.y));
    vec2 ds = win / u_view.zw * vec2(256.0, 384.0);   // DS pixels over both screens, y down
    if (ds.x < 0.0 || ds.y < 0.0 || ds.x >= 256.0 || ds.y >= 384.0) { o_col = vec4(0.0, 0.0, 0.0, 1.0); return; }
    ivec2 p = ivec2(ds);
    uvec2 w = texelFetch(u_words, p, 0).rg;
    bool upper = p.y < 192;
    bool isA = upper == (u_atop != 0);
    uint c = w.x & 0x3FFFFu, mode = w.x >> 24 & 3u;
    uint c3 = 0u, a3 = 0u;
    if (isA && mode != 0u) {
        vec2 uv = vec2(ds.x / 256.0, 1.0 - (ds.y - (upper ? 0.0 : 192.0)) / 192.0);
        vec4 t = texture(u_3d, uv);
        uvec3 q = uvec3(round(t.rgb * 63.0));
        c3 = q.r | q.g << 6 | q.b << 12;
        a3 = uint(round(t.a * 31.0));
    }
    if (mode == 1u) {
        if (a3 != 0u) {
            if ((w.x & (1u << 26)) != 0u) {
                uint eva = a3 + 1u, evb = 32u - eva, o = 0u;
                for (int s = 0; s < 18; s += 6) o |= (((c3 >> s & 63u) * eva + (c >> s & 63u) * evb) >> 5) << s;
                c = o;
            } else {
                c = c3;
            }
            uint bm = w.y >> 21 & 3u, evy = w.y >> 16 & 31u;
            if (bm == 2u) c = brighter(c, evy);
            else if (bm == 3u) c = darker(c, evy);
        }
    } else if (mode == 2u && a3 != 0u) {
        c = blend(c, c3, w.y & 31u, w.y >> 8 & 31u);
    }
    uint mm = w.y >> 24 & 3u, mf = w.y >> 26 & 31u;
    if (mm == 1u) c = brighter(c, mf);
    else if (mm == 2u) c = darker(c, mf);
    uvec3 rgb = uvec3(c & 63u, c >> 6 & 63u, c >> 12 & 63u);
    rgb = rgb << 2 | rgb >> 4;
    o_col = vec4(vec3(rgb) / 255.0, 1.0);
}
)";

bool gl_load() {
    bool ok = true;
#define NDSKIT_GL_LOAD(T, n)                                                   \
    n = reinterpret_cast<T>(SDL_GL_GetProcAddress(#n));                        \
    if (!n) { nds_note("display: no %s", #n); ok = false; }
    NDSKIT_GL_FUNCS(NDSKIT_GL_LOAD)
#undef NDSKIT_GL_LOAD
    return ok;
}

// The 2D words of both screens, upper first.
void gather_words() {
    bool atop = gpu2d_top_is_a();
    for (int s = 0; s < 2; ++s) {
        int e = (s == 0) == atop ? 0 : 1;
        std::memcpy(&words[(size_t)s * 192 * 256 * 2], &g_2d.px[e][0][0][0], 192 * 256 * 8);
    }
}

// The rectangle of the window the screens fill.
void view_rect(int ww, int wh, float* r) {
    float s = std::min(ww / 256.0f, wh / 384.0f);
    r[2] = 256 * s; r[3] = 384 * s;
    r[0] = (ww - r[2]) / 2; r[1] = (wh - r[3]) / 2;
}

void read_input() {
    const bool* k = SDL_GetKeyboardState(nullptr);
    struct { SDL_Scancode sc; uint32_t bit; } map[] = {
        {SDL_SCANCODE_X, 1u << 0}, {SDL_SCANCODE_Z, 1u << 1}, {SDL_SCANCODE_BACKSPACE, 1u << 2},
        {SDL_SCANCODE_RETURN, 1u << 3}, {SDL_SCANCODE_RIGHT, 1u << 4}, {SDL_SCANCODE_LEFT, 1u << 5},
        {SDL_SCANCODE_UP, 1u << 6}, {SDL_SCANCODE_DOWN, 1u << 7}, {SDL_SCANCODE_W, 1u << 8},
        {SDL_SCANCODE_Q, 1u << 9}, {SDL_SCANCODE_S, 1u << 10}, {SDL_SCANCODE_A, 1u << 11}};
    uint32_t keys = 0;
    for (auto& m : map)
        if (k[m.sc]) keys |= m.bit;
    float mx, my;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    int lw, lh;
    SDL_GetWindowSize(win, &lw, &lh);
    float px = mx * ww / std::max(lw, 1), py = my * wh / std::max(lh, 1);
    float r[4];
    view_rect(ww, wh, r);
    int dx = (int)((px - r[0]) / r[2] * 256), dy = (int)((py - r[1]) / r[3] * 384) - 192;
    bool pen = (mb & SDL_BUTTON_LMASK) && dx >= 0 && dx < 256 && dy >= 0 && dy < 192;
    uint16_t kin = (uint16_t)(~keys & 0x3FF);
    uint16_t ext = (uint16_t)(0x7F & ~((keys >> 10) & 3) & ~(pen ? 0x40 : 0));
    mmio_set_keys(kin, ext);
    spi_set_pen(pen, pen ? dx : 0, pen ? dy : 0);
}

}  // namespace

GLuint gl_program(const char* vs, const char* fs) {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            nds_note("display: a shader does not compile:\n%s", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        nds_note("display: a program does not link: %s", log);
        return 0;
    }
    return p;
}

bool display_open(bool vis) {
    visible = vis;
    if (!SDL_Init(SDL_INIT_VIDEO)) { nds_note("display: SDL: %s", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int wh = 768;
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) wh = std::max(384, (int)(dm->h * 0.85f));
    std::string title = "ndskit - " + g_cfg.rom.substr(g_cfg.rom.find_last_of("/\\") + 1);
    win = SDL_CreateWindow(title.c_str(), wh / 2, wh,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (vis ? 0 : SDL_WINDOW_HIDDEN));
    if (!win) { nds_note("display: no window: %s", SDL_GetError()); return false; }
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { nds_note("display: no OpenGL 4.5 context: %s", SDL_GetError()); return false; }
    if (!gl_load()) return false;
    if (!render_init()) return false;
    comp_prog = gl_program(kCompVS, kCompFS);
    if (!comp_prog) return false;
    loc_view = glGetUniformLocation(comp_prog, "u_view");
    loc_atop = glGetUniformLocation(comp_prog, "u_atop");
    glCreateTextures(GL_TEXTURE_2D, 1, &words_tex);
    glTextureStorage2D(words_tex, 1, GL_RG32UI, 256, 384);
    glCreateVertexArrays(1, &empty_vao);
    render_target(t_one, 1);
    if (vis) {
        float hz = 0;
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win))) hz = dm->refresh_rate;
        vsync = hz >= 50.0f && SDL_GL_SetSwapInterval(1);
        if (!vsync) SDL_GL_SetSwapInterval(0);
        refresh_ns = hz > 0 ? 1e9 / hz : kFrameNs;
        nds_note("display: %s, %.2f Hz, %s", (const char*)glGetString(GL_RENDERER), hz,
                 vsync ? "with the vertical sync" : "paced by the clock");
        next_ns = SDL_GetTicksNS();
    }
    return true;
}

bool display_frame() {
    SDL_Event ev;
    bool shot = false;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return false;
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            if (ev.key.scancode == SDL_SCANCODE_ESCAPE) return false;
            if (ev.key.scancode == SDL_SCANCODE_F12) shot = true;
        }
    }
    if (g_cfg.input.empty()) read_input();     // a script drives the input instead
    if (shot) display_shot(g_cfg.out + "/shot" + std::to_string(++shots_taken) + ".png");

    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    float r[4];
    view_rect(ww, wh, r);
    int scale = g_cfg.scale > 0 ? g_cfg.scale : std::max(1, (int)std::lround(r[3] / 384.0f));
    render_target(t_win, scale);
    GLuint pic = render_draw(gx_job(), t_win);
    gather_words();
    glTextureSubImage2D(words_tex, 0, 0, 0, 256, 384, GL_RG_INTEGER, GL_UNSIGNED_INT, words.data());
    auto present = [&]() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        glUseProgram(comp_prog);
        glProgramUniform4f(comp_prog, loc_view, r[0], r[1], r[2], r[3]);
        glProgramUniform1i(comp_prog, loc_atop, gpu2d_top_is_a() ? 1 : 0);
        glBindTextureUnit(0, words_tex);
        glBindTextureUnit(1, pic);
        glBindVertexArray(empty_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        if (!capture.empty()) {
            std::vector<uint8_t> rgb((size_t)ww * wh * 3), flip(rgb.size());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
            for (int y = 0; y < wh; ++y) std::memcpy(&flip[(size_t)y * ww * 3], &rgb[(size_t)(wh - 1 - y) * ww * 3], (size_t)ww * 3);
            write_png(capture, ww, wh, flip.data());
            capture.clear();
        }
        SDL_GL_SwapWindow(win);
    };
    present();
    // the next DS frame is due at next_ns: with the vertical sync, the same
    // picture again at each of the display's refreshes until then (a display
    // at 120 Hz shows each frame twice); without it, a wait
    next_ns += (uint64_t)kFrameNs;
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        if (now > next_ns + 100000000ull) { next_ns = now; break; }   // far behind: do not run to catch up
        if (!vsync) { if (next_ns > now) SDL_DelayPrecise(next_ns - now); break; }
        if (now + (uint64_t)(refresh_ns / 2) >= next_ns) break;
        present();
    }
    return true;
}

bool display_read3d(uint32_t* out) {
    if (!win) return false;
    GLuint pic = render_draw(gx_job(), t_one);
    std::vector<uint8_t> px3d(256 * 192 * 4);
    glGetTextureImage(pic, 0, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)px3d.size(), px3d.data());
    auto u6 = [](uint8_t v) { return (uint32_t)((v * 63 + 127) / 255); };
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 256; ++x) {
            const uint8_t* q = &px3d[((size_t)(191 - y) * 256 + x) * 4];
            out[y * 256 + x] = u6(q[0]) | u6(q[1]) << 6 | u6(q[2]) << 12 | (uint32_t)((q[3] * 31 + 127) / 255) << 18;
        }
    return true;
}

void display_shot(const std::string& path) {
    if (visible) capture = path.substr(0, path.size() - 4) + "-window.png";
    GLuint pic = render_draw(gx_job(), t_one);
    std::vector<uint8_t> px3d(256 * 192 * 4);
    glGetTextureImage(pic, 0, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)px3d.size(), px3d.data());
    std::vector<uint8_t> rgb(256 * 384 * 3);
    bool a_top = gpu2d_top_is_a();
    for (int s = 0; s < 2; ++s) {
        int e = (s == 0) == a_top ? 0 : 1;
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 256; ++x) {
                uint32_t p3 = 0;
                if (e == 0) {
                    const uint8_t* q = &px3d[((size_t)(191 - y) * 256 + x) * 4];
                    auto u6 = [](uint8_t v) { return (uint32_t)((v * 63 + 127) / 255); };
                    p3 = u6(q[0]) | u6(q[1]) << 6 | u6(q[2]) << 12 | (uint32_t)((q[3] * 31 + 127) / 255) << 18;
                }
                uint32_t c = gpu2d_compose(g_2d.px[e][y][x], p3);
                uint8_t* p = &rgb[((size_t)(s * 192 + y) * 256 + x) * 3];
                p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c;
            }
    }
    write_png(path, 256, 384, rgb.data());
}
