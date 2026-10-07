// ndskit runtime — input from a script, for headless runs: buttons and the
// stylus at given VBlanks.
//
//     --input "VBL:BUTTONS[:FRAMES],VBL:tap:X:Y[:FRAMES],..."
//
// BUTTONS are DS names joined by '+' (A B X Y L R START SELECT UP DOWN LEFT
// RIGHT); a tap holds the stylus at (X, Y) of the lower screen. Each lasts
// FRAMES VBlanks (9 by default, about the oracle's 0.15 s press).
#include "nds.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

void spi_set_pen(bool down, int x, int y);

struct Press { uint64_t from, to; uint32_t keys; bool tap; int x, y; };
static std::vector<Press> g_script;

static uint32_t key_bit(const std::string& k) {
    static const char* names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    for (int i = 0; i < 12; ++i)
        if (k == names[i]) return 1u << i;              // bits 10, 11: X and Y (EXTKEYIN 0, 1)
    nds_fatal("input: no button %s", k.c_str());
}

void input_parse(const std::string& s) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        std::vector<std::string> f;
        std::stringstream is(item);
        std::string x;
        while (std::getline(is, x, ':')) f.push_back(x);
        if (f.size() < 2) nds_fatal("input: %s is not VBL:WHAT", item.c_str());
        Press p{};
        p.from = std::strtoull(f[0].c_str(), nullptr, 10);
        size_t dur = 2;
        if (f[1] == "tap") {
            if (f.size() < 4) nds_fatal("input: %s: tap:X:Y", item.c_str());
            p.tap = true; p.x = std::atoi(f[2].c_str()); p.y = std::atoi(f[3].c_str());
            dur = 4;
        } else {
            std::stringstream ks(f[1]);
            std::string k;
            while (std::getline(ks, k, '+')) p.keys |= key_bit(k);
        }
        p.to = p.from + (f.size() > dur ? std::strtoull(f[dur].c_str(), nullptr, 10) : 9);
        g_script.push_back(p);
    }
}

// At each VBlank: what is held now.
void input_vblank(uint64_t vbl) {
    if (g_script.empty()) return;
    uint32_t keys = 0;
    bool pen = false;
    int x = 0, y = 0;
    for (const Press& p : g_script) {
        if (vbl < p.from || vbl >= p.to) continue;
        keys |= p.keys;
        if (p.tap) { pen = true; x = p.x; y = p.y; }
    }
    uint16_t k = (uint16_t)(~keys & 0x3FF);
    uint16_t ext = (uint16_t)(0x7F & ~((keys >> 10) & 3) & ~(pen ? 0x40 : 0));
    mmio_set_keys(k, ext);
    spi_set_pen(pen, x, y);
}
