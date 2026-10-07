// ndskit runtime — what the geometry engine (gx.cpp) hands the rendering
// engine (render_gl.cpp): a frame's polygons, already transformed, lit and
// clipped, with the 3D registers and the texture memory as they were when
// the DS started drawing it.
#pragma once
#include <cstdint>
#include <vector>

struct GxVertex {
    int32_t x, y, z, w;                 // clip coordinates (20.12), after clipping
    float sx, sy;                       // the screen position, exact (pixels, y down)
    int16_t ix, iy;                     // ... as the DS rounds it
    float r, g, b;                      // the colour, 0-31 (fractions where clipping interpolated)
    float s, t;                         // texture coordinates, in texels
};

struct GxPoly {
    uint32_t first, n;                  // its vertices in the frame's list
    uint32_t attr;                      // POLYGON_ATTR
    uint32_t tex, pal;                  // TEXIMAGE_PARAM, PLTT_BASE
    bool translucent;
    uint32_t sortkey;                   // bottom y << 8 | top y
};

struct Render3dJob {
    bool valid = false;
    uint64_t frame = 0;                 // the swap it came from (counts swaps)
    std::vector<GxVertex> verts;
    std::vector<GxPoly> polys;          // in the order the rendering engine draws them
    bool wbuffer = false;               // SWAP_BUFFERS bit 1: depth is w
    uint16_t disp3dcnt = 0;
    uint32_t clear_color = 0;           // CLEAR_COLOR: colour, fog bit, alpha, polygon ID
    uint16_t clear_depth = 0x7FFF;
    uint16_t clrimage_offset = 0;
    uint8_t alpha_ref = 0;
    uint16_t edge[8] = {};
    uint32_t fog_color = 0;
    uint16_t fog_offset = 0;
    uint8_t fog_table[32] = {};
    uint16_t toon[32] = {};
    std::vector<uint8_t> tex;           // the texture slots, 512 KiB
    std::vector<uint8_t> texpal;        // the texture-palette slots, 96 KiB
};

// the job the rendering engine is drawing now (the picture of the frame being shown)
const Render3dJob& gx_job();
