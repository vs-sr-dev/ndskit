// ndskit runtime — ndsrun: a cartridge's recompiled programs on the runtime,
// headless.
//
//     ndsrun GAME.nds [--out DIR] [--vblanks N] [--save FILE] [--trace]
//                     [--ticks9 N] [--ticks7 N] [--no-dump] [--calls N]
//                     [--break ADDR | --break7 ADDR] [--watch ADDR[:LEN]]
//
// Runs until N VBlanks have passed (or a fatal error), then writes the
// memories to DIR (main.bin, itcm.bin, dtcm.bin, wram7.bin, swram.bin) and
// the CPUs' registers (cpus.txt), for tools/compare against an oracle.
#include "nds.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

int main(int argc, char** argv) {
    NdsConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--out") cfg.out = next();
        else if (a == "--vblanks") cfg.stop_vblanks = std::strtoull(next().c_str(), nullptr, 0);
        else if (a == "--save") cfg.save = next();
        else if (a == "--trace") cfg.trace = true;
        else if (a == "--ticks9") cfg.ticks9 = std::atoi(next().c_str());
        else if (a == "--ticks7") cfg.ticks7 = std::atoi(next().c_str());
        else if (a == "--no-dump") cfg.dump = false;
        else if (a == "--calls") cfg.calls = std::atoi(next().c_str());
        else if (a == "--input") cfg.input = next();
        else if (a == "--watch") {                  // ADDR[:LEN], hex
            std::string w = next();
            size_t k = w.find(':');
            cfg.watch_lo = (uint32_t)std::strtoul(w.substr(0, k).c_str(), nullptr, 16);
            cfg.watch_len = k == std::string::npos ? 4 : (uint32_t)std::strtoul(w.substr(k + 1).c_str(), nullptr, 16);
        }
        else if (a == "--break" || a == "--break7") {
            cfg.break_cpu = a == "--break" ? 9 : 7;
            cfg.break_at = (uint32_t)std::strtoul(next().c_str(), nullptr, 16);
            if (!cfg.calls) cfg.calls = 64;
        }
        else if (!a.empty() && a[0] != '-' && cfg.rom.empty()) cfg.rom = a;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (cfg.rom.empty()) {
        std::fprintf(stderr, "usage: ndsrun GAME.nds [--out DIR] [--vblanks N] [--save FILE] [--trace]\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg.out, ec);
    return nds_main(cfg);
}
