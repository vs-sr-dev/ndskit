// ndskit runtime — the recompiler's self-test: recompiled functions run on
// the vectors ndskit/recomp/selftest.py recorded with the interpreter
// (armemu), and every result compared.
//
//     selftest VECTORS.txt ...
//
// A vectors file is lines of:
//     cpu 9|7                 the CPU of what follows (its memory map; memory cleared)
//     image BASE PATH         load a file at BASE (hex) into that CPU's memory
//     module NAME             activate that module (its segments must be in memory)
//     func KEY LABEL          the function the next vectors call (KEY: address, bit 0 Thumb)
//     v IN... OUT...          16 hex words each: r0-r14, cpsr (lr in: the return sentinel)
//     mem CRC...              crc32 of main RAM, then of the ITCM and DTCM (ARM9) or of
//                             the ARM7's WRAM and the shared WRAM, after the function's vectors
// Memory is shared by all vectors in order, as in the interpreter's run.
#include "arm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const uint32_t kSentinel = 0xFFFFFFF0u;     // armemu.RETURN_SENTINEL
static const char* kNames[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
                                 "r11", "r12", "sp", "lr", "cpsr"};

// The stub services end the run on a hardware access or a bad call: say
// which vector it was.
static std::string g_where;
static void where() {
    if (!g_where.empty()) std::fprintf(stderr, "  during %s\n", g_where.c_str());
}

static void put(ArmContext& c, const uint32_t* w, int cpu) {
    std::memset(&c, 0, sizeof c);
    c.cpu = cpu;
    c.mode = 0x1F;
    arm_set_cpsr(c, w[15], 0xF);
    for (int i = 0; i < 15; ++i) c.r[i] = w[i];
}

static void get(const ArmContext& c, uint32_t* w) {
    for (int i = 0; i < 15; ++i) w[i] = c.r[i];
    w[15] = arm_get_cpsr(c);
}

static std::vector<uint32_t> crcs(int cpu) {
    std::vector<uint32_t> out{nds_crc32(g_main, 0x400000)};
    if (cpu == 9) {
        out.push_back(nds_crc32(g_itcm, 0x8000));
        out.push_back(nds_crc32(g_dtcm, 0x4000));
    } else {
        out.push_back(nds_crc32(g_wram7, 0x10000));
        out.push_back(nds_crc32(g_swram, 0x8000));
    }
    return out;
}

static void clear() {
    std::memset(g_main, 0, 0x400000);
    std::memset(g_itcm, 0, 0x8000);
    std::memset(g_dtcm, 0, 0x4000);
    std::memset(g_wram7, 0, 0x10000);
    std::memset(g_swram, 0, 0x8000);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: selftest VECTORS.txt ...\n");
        return 2;
    }
    std::atexit(where);
    long total_funcs = 0, total_vec = 0, total_bad = 0;
    for (int fi = 1; fi < argc; ++fi) {
        std::ifstream in(argv[fi]);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[fi]); return 2; }
        std::string line, label;
        uint32_t key = 0;
        int cpu = 9;
        long funcs = 0, vec = 0, bad = 0, fbad = 0, fvec = 0;
        clear();
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string kw;
            ss >> kw;
            if (kw.empty() || kw[0] == '#') continue;
            if (kw == "cpu") {
                ss >> cpu;
                clear();
            } else if (kw == "image") {
                std::string base, path;
                ss >> base >> std::ws;
                std::getline(ss, path);
                std::ifstream img(path, std::ios::binary);
                std::vector<char> data((std::istreambuf_iterator<char>(img)), std::istreambuf_iterator<char>());
                if (data.empty() || !nds_mem_write(cpu, (uint32_t)std::stoul(base, nullptr, 16), data.data(), data.size())) {
                    std::fprintf(stderr, "cannot load %s at %s\n", path.c_str(), base.c_str());
                    return 2;
                }
            } else if (kw == "module") {
                std::string name;
                ss >> name;
                const ArmModule* m = arm_module(name.c_str());
                if (!m) { std::fprintf(stderr, "no module %s in this build\n", name.c_str()); return 2; }
                if (!arm_identify(m)) {
                    std::fprintf(stderr, "the memory does not hold module %s as it was built\n", name.c_str());
                    return 2;
                }
                arm_activate(m);
            } else if (kw == "func") {
                std::string e;
                ss >> e >> std::ws;
                std::getline(ss, label);
                key = (uint32_t)std::stoul(e, nullptr, 16);
                ++funcs; fbad = 0; fvec = 0;
            } else if (kw == "v") {
                uint32_t w[32], got[16];
                for (auto& x : w) { std::string h; ss >> h; x = (uint32_t)std::stoul(h, nullptr, 16); }
                ArmContext c;
                put(c, w, cpu);
                c.r[14] = kSentinel; c.pc = 0; c.budget = 1 << 20;
                g_where = std::string(argv[fi]) + ": " + label + ", vector " + std::to_string(fvec + 1);
                arm_call(c, key);
                g_where.clear();
                get(c, got);
                ++vec; ++fvec;
                bool ok = c.pc == kSentinel && !std::memcmp(got, w + 16, sizeof got);
                if (!ok) {
                    ++bad;
                    if (fbad++ < 3) {
                        std::printf("FAIL %08X %s, vector %ld:", key, label.c_str(), fvec);
                        if (c.pc != kSentinel) std::printf(" returned to %08X", c.pc);
                        for (int i = 0; i < 16; ++i)
                            if (got[i] != w[16 + i]) std::printf(" %s=%08X (want %08X)", kNames[i], got[i], w[16 + i]);
                        std::printf("\n      in:");
                        for (int i = 0; i < 16; ++i) std::printf(" %08X", w[i]);
                        std::printf("\n");
                    }
                }
            } else if (kw == "mem") {
                std::vector<uint32_t> want;
                std::string h;
                while (ss >> h) want.push_back((uint32_t)std::stoul(h, nullptr, 16));
                std::vector<uint32_t> have = crcs(cpu);
                if (want != have) {
                    ++bad;
                    std::printf("FAIL %08X %s: memory differs after its vectors (", key, label.c_str());
                    for (size_t i = 0; i < have.size() && i < want.size(); ++i)
                        std::printf("%s%s", i ? ", " : "", have[i] == want[i] ? "same" : "differs");
                    std::printf(")\n");
                    // going on from the interpreter's state is not possible: stop this file
                    break;
                }
            }
        }
        std::printf("%s: %ld functions, %ld vectors, %ld failures\n", argv[fi], funcs, vec, bad);
        total_funcs += funcs; total_vec += vec; total_bad += bad;
    }
    if (argc > 2)
        std::printf("total: %ld functions, %ld vectors, %ld failures\n", total_funcs, total_vec, total_bad);
    return total_bad ? 1 : 0;
}
