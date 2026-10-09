// ndskit runtime — ndsrun: a cartridge's recompiled programs on the runtime.
//
//     ndsrun GAME.nds [--window] [--scale N] [--mute] [--wav FILE] [--out DIR] [--vblanks N]
//                     [--save FILE] [--shots V,V,...] [--shot-every N] [--input SCRIPT] [--trace]
//                     [--ticks9 N] [--ticks7 N] [--no-dump] [--calls N]
//                     [--break ADDR | --break7 ADDR] [--watch ADDR[:LEN]]
//
// With --window, the game in a window, in real time, played with the
// keyboard and the mouse, heard on the host's audio device (unless
// --mute). Otherwise headless: it runs until N VBlanks have passed (or a
// fatal error), writes both screens as PNGs at the VBlanks asked for
// (DIR/v<N>.png), then writes the memories to DIR (main.bin, itcm.bin,
// dtcm.bin, wram7.bin, swram.bin) and the CPUs' registers (cpus.txt), for
// comparisons with an oracle. Either way --wav writes the sound to a file.
#include "display.h"
#include "nds.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

int main(int argc, char** argv) {
    NdsConfig cfg;
    bool mute = false;
    std::string wav;
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
        else if (a == "--window") cfg.window = true;
        else if (a == "--scale") cfg.scale = std::atoi(next().c_str());
        else if (a == "--mute") mute = true;
        else if (a == "--wav") wav = next();
        else if (a == "--shot-every") cfg.shot_every = std::strtoull(next().c_str(), nullptr, 0);
        else if (a == "--shots") {
            std::stringstream ss(next());
            std::string v;
            while (std::getline(ss, v, ',')) if (!v.empty()) cfg.shots.push_back(std::strtoull(v.c_str(), nullptr, 0));
        }
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
        std::fprintf(stderr, "usage: ndsrun GAME.nds [--window] [--out DIR] [--vblanks N] [--shots V,...]\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg.out, ec);
    if (!nds_boot(cfg)) return 2;
    bool shots = !cfg.shots.empty() || cfg.shot_every;
    if ((cfg.window || shots) && !display_open(cfg.window)) return 2;
    if (!audio_open(cfg.window && !mute, wav)) return 2;
    auto shot_at = [&](uint64_t vbl) {
        return shots && ((cfg.shot_every && vbl % cfg.shot_every == 0) ||
                         std::find(cfg.shots.begin(), cfg.shots.end(), vbl) != cfg.shots.end());
    };
    for (;;) {
        g_2d_on = cfg.window || shot_at(nds_vblanks() + 1);   // headless, the 2D engines draw only what a picture shows
        if (!nds_run_frame()) break;
        uint64_t vbl = nds_vblanks();
        audio_frame();
        if (shot_at(vbl))
            display_shot(cfg.out + "/v" + std::to_string(vbl) + ".png");
        if (cfg.window && !display_frame()) break;
    }
    audio_close();
    nds_finish();
}
