// ndskit runtime — the ARM7's sound unit: 16 channels, 2 capture units, the
// mixer.
//
// The unit makes a stereo sample every 1 024 system cycles (2 048 ticks,
// 32 728.5 Hz). It runs in virtual time (sound_sync): up to the machine's
// time after each quantum (a quarter of a line, half a sample), and up to the
// ARM7's at each access to its registers. So a run is the same run every
// time, with or without anyone listening, a channel's busy bit ends when its
// samples do, and a channel reads memory as it stands at its own time: run
// only now and then, it read later than it played, and a stream the ARM9
// refills behind the play position came out with the next lap's samples.
//
// Each output sample, every channel's timer (the system clock / 2, counting
// up from the channel's reload) advances 512, and each overflow steps the
// channel: PCM8 and PCM16 read the next sample, IMA-ADPCM the next nibble
// (the first word is its header: the value and the step index; the state
// after the loop's first nibble is kept for a loop to go back to), PSG (8-13) the
// next eighth of a square wave of the duty given, noise (14-15) the next bit
// of a 15-bit LFSR. A sampled channel starts 3 steps late (ADPCM spends the
// first 8 nibbles' time on its header); with less than 16 bytes in all it
// plays nothing. A one-shot channel ends after its last sample: silent, the
// busy bit cleared. The samples come through a FIFO as the hardware's do: 32
// bytes fetched as the channel starts, 16 more each time it is down to 16.
// A stream refilled behind the play position counts on that read-ahead
// (Moon's movies rewrite each 256-byte block of their ring buffer the
// moment the play position leaves it: read at the moment of playing, the
// block's last samples were already the next lap's).
//
// The mix, in integers at the hardware's widths (GBATEK; melonDS's
// arithmetic): a channel's sample times 16 >> the divider's shift (0, 1, 2,
// 4) times its volume (0-127, 127 as 128); panned by (128 - pan) and pan
// (127 as 128), >> 10; channels 1 and 3 left out of the mixer by SOUNDCNT
// bits 12-13; capture 0 takes the left mixer (or channel 0), capture 1 the
// right (or channel 2), at the timer of channel 1 or 3, as PCM8 or PCM16 into
// the ARM7's memory; each output is the mixer, channel 1, 3 or both (SOUNDCNT
// bits 8-11); times the master volume (127 as 128) >> 7, >> 8, plus the bias
// << 6, centred on 0x200's (the bias every SDK sets), clipped to 16 bits.
// Silent unless POWCNT2 bit 0 (the speakers) and SOUNDCNT bit 15. The DS
// truncates the output to 10 bits: kept at 16 here.
#include "nds.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static uint8_t g_regs[0x120];                       // 0x04000400-0x0400051F, as read back

namespace {

struct Channel {
    uint32_t cnt, src, loop, len;                   // loop and length in bytes
    uint16_t reload;
    bool key_on;                                    // started: takes effect at the next step
    uint32_t timer;
    int32_t pos;                                    // sample (ADPCM: nibble) index; < 0 before the first
    int32_t cur;                                    // the sample being output
    uint16_t lfsr;
    int32_t adpcm, index, adpcm_loop, index_loop;
    uint8_t byte;                                   // ADPCM: the byte whose nibbles play
    uint8_t fifo[32];                               // the samples fetched ahead
    uint32_t fifo_rd, fifo_wr, fifo_level;          // bytes
    uint32_t fetch;                                 // the next fetch's offset from the source
};

struct Capture {
    uint8_t cnt;
    uint32_t dst, len;                              // bytes
    uint16_t reload;
    uint32_t timer, pos;
};

Channel ch[16];
Capture cap[2];
uint64_t g_next;                                    // the time of the next output sample
std::vector<int16_t> g_out;

const int16_t kAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
const int8_t kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

uint32_t reg32(uint32_t off) { uint32_t v; std::memcpy(&v, &g_regs[off], 4); return v; }
uint16_t reg16(uint32_t off) { uint16_t v; std::memcpy(&v, &g_regs[off], 2); return v; }

int fmt(const Channel& c) { return c.cnt >> 29 & 3; }

// The FIFO: a burst of 16 bytes from where the fetches stand (back to the
// loop start past the end; nothing past a one-shot's end).
void fetch(Channel& c) {
    uint32_t end = c.loop + c.len;
    if (c.fetch >= end) {
        if (c.cnt & 1u << 27) c.fetch = c.loop;
        else return;
    }
    uint32_t n = std::min(16u, end - c.fetch);
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        if (uint8_t* p = nds_fast<7>(c.src + c.fetch)) std::memcpy(&w, p, 4);   // memory only (VRAM: an inference)
        std::memcpy(&c.fifo[c.fifo_wr], &w, 4);
        c.fifo_wr = (c.fifo_wr + 4) & 31;
        c.fetch += 4;
    }
    c.fifo_level += n;
}

// The next sample's bytes from the FIFO, which fetches again once half empty.
template <class T> T take(Channel& c) {
    T v;
    std::memcpy(&v, &c.fifo[c.fifo_rd], sizeof v);
    c.fifo_rd = (c.fifo_rd + sizeof v) & 31;
    c.fifo_level -= sizeof v;
    if (c.fifo_level <= 16) fetch(c);
    return v;
}

void start(Channel& c) {
    c.timer = c.reload;
    c.pos = fmt(c) == 3 ? -1 : -3;
    c.cur = 0;
    c.lfsr = 0x7FFF;
    c.fifo_rd = c.fifo_wr = c.fifo_level = c.fetch = 0;
    if (fmt(c) != 3) { fetch(c); fetch(c); }        // 32 bytes as the channel starts
}

void stop(Channel& c, int n) {
    c.cur = 0;
    c.cnt &= 0x7FFFFFFFu;
    g_regs[0x10 * n + 3] &= 0x7F;
    io_set16(7, 0x04000402u + 0x10u * n, reg16(0x10 * n + 2));
}

// One step of a channel: its next sample (or the end of a one-shot).
void step(Channel& c, int n) {
    uint32_t end = c.loop + c.len;
    switch (fmt(c)) {
    case 0:
        if (++c.pos < 0) return;
        if ((uint32_t)c.pos >= end) {
            if (c.cnt & 1u << 27) c.pos = (int32_t)c.loop;
            else { stop(c, n); return; }
        }
        c.cur = take<int8_t>(c) * 256;
        return;
    case 1:
        if (++c.pos < 0) return;
        if ((uint32_t)c.pos * 2 >= end) {
            if (c.cnt & 1u << 27) c.pos = (int32_t)(c.loop / 2);
            else { stop(c, n); return; }
        }
        c.cur = take<int16_t>(c);
        return;
    case 2: {
        if (++c.pos < 8) {
            if (c.pos == 0) {
                uint32_t h = take<uint32_t>(c);
                c.adpcm = c.adpcm_loop = (int16_t)h;
                c.index = c.index_loop = std::min<int32_t>(h >> 16 & 0x7F, 88);
            }
            return;
        }
        if ((uint32_t)c.pos / 2 >= end) {           // back to the state kept after the loop's first nibble
            if (!(c.cnt & 1u << 27)) { stop(c, n); return; }
            c.pos = (int32_t)(c.loop * 2);
            c.cur = c.adpcm = c.adpcm_loop;
            c.index = c.index_loop;
            c.byte = take<uint8_t>(c);
            return;
        }
        if (c.pos & 1) c.byte >>= 4;
        else c.byte = take<uint8_t>(c);
        uint32_t nib = c.byte & 15;
        int32_t s = kAdpcmStep[c.index], d = s >> 3;
        if (nib & 1) d += s >> 2;
        if (nib & 2) d += s >> 1;
        if (nib & 4) d += s;
        c.adpcm = nib & 8 ? std::max(c.adpcm - d, -0x7FFF) : std::min(c.adpcm + d, 0x7FFF);
        c.index = std::clamp(c.index + kAdpcmIndex[nib & 7], 0, 88);
        if ((uint32_t)c.pos == c.loop * 2) { c.adpcm_loop = c.adpcm; c.index_loop = c.index; }
        c.cur = c.adpcm;
        return;
    }
    default:
        if (n >= 14) {                              // noise
            if (c.lfsr & 1) { c.lfsr = (uint16_t)((c.lfsr >> 1) ^ 0x6000); c.cur = -0x7FFF; }
            else { c.lfsr >>= 1; c.cur = 0x7FFF; }
        } else if (n >= 8) {                        // PSG: duty (d + 1) / 8 high, 7 is always low
            ++c.pos;
            int duty = c.cnt >> 24 & 7;
            c.cur = duty < 7 && (c.pos & 7) >= 7 - duty ? 0x7FFF : -0x7FFF;
        }
        return;
    }
}

// A channel through one output sample: its value after volume, before panning.
int32_t run(Channel& c, int n) {
    if (!(c.cnt & 0x80000000u)) return 0;
    if (fmt(c) != 3 && c.loop + c.len < 16) return 0;
    if (fmt(c) == 3 && n < 8) return 0;
    if (c.key_on) { start(c); c.key_on = false; }
    c.timer += 512;
    while (c.timer >> 16) {
        c.timer = c.reload + (c.timer - 0x10000u);
        step(c, n);
        if (!(c.cnt & 0x80000000u)) break;
    }
    static const int kShift[4] = {4, 3, 2, 0};
    int vol = c.cnt & 0x7F;
    return (c.cur << kShift[c.cnt >> 8 & 3]) * (vol == 127 ? 128 : vol);
}

int pan(const Channel& c) { int p = c.cnt >> 16 & 0x7F; return p == 127 ? 128 : p; }

void capture(Capture& k, int32_t v) {
    k.timer += 512;
    while (k.timer >> 16) {
        k.timer = k.reload + (k.timer - 0x10000u);
        uint32_t a = k.dst + k.pos;
        if (k.cnt & 8) { nds7::st8(a, (uint32_t)(v >> 8)); k.pos += 1; }
        else { nds7::st16(a & ~1u, (uint32_t)v); k.pos += 2; }
        if (k.pos >= k.len) {
            if (k.cnt & 4) {
                k.cnt &= 0x7F;
                g_regs[0x108 + (&k - cap)] = k.cnt;
                io_set16(7, 0x04000508u, reg16(0x108));
                return;
            }
            k.pos = 0;
        }
    }
}

void mix_one() {
    uint16_t sc = reg16(0x100);
    int32_t left = 0, right = 0, out_l = 0, out_r = 0;
    if (sc & 0x8000) {
        int32_t v[16];
        for (int n = 0; n < 16; ++n) {
            v[n] = run(ch[n], n);
            if (!v[n]) continue;
            if ((n == 1 && (sc & 0x1000)) || (n == 3 && (sc & 0x2000))) continue;
            int p = pan(ch[n]);
            left += (int32_t)(((int64_t)v[n] * (128 - p)) >> 10);
            right += (int32_t)(((int64_t)v[n] * p) >> 10);
        }
        for (int k = 0; k < 2; ++k) {
            if (!(cap[k].cnt & 0x80)) continue;
            int32_t s = cap[k].cnt & 2 ? v[2 * k] : (k ? right : left);
            capture(cap[k], std::clamp(s >> 8, -0x8000, 0x7FFF));
        }
        auto one = [&](int n, bool r) {
            int p = pan(ch[n]);
            return (int32_t)(((int64_t)v[n] * (r ? p : 128 - p)) >> 10);
        };
        switch (sc >> 8 & 3) {
        case 0: out_l = left; break;
        case 1: out_l = one(1, false); break;
        case 2: out_l = one(3, false); break;
        case 3: out_l = one(1, false) + one(3, false); break;
        }
        switch (sc >> 10 & 3) {
        case 0: out_r = right; break;
        case 1: out_r = one(1, true); break;
        case 2: out_r = one(3, true); break;
        case 3: out_r = one(1, true) + one(3, true); break;
        }
    }
    if (!g_sound_keep) return;
    int mv = sc & 0x7F;
    if (mv == 127) mv = 128;
    int32_t bias = ((reg16(0x104) & 0x3FF) - 0x200) << 6;
    bool on = (sc & 0x8000) && (io_reg16(7, 0x04000304u) & 1);
    for (int32_t o : {out_l, out_r}) {
        int32_t s = (int32_t)(((int64_t)o * mv) >> 7 >> 8) + bias;
        g_out.push_back(on ? (int16_t)std::clamp(s, -0x8000, 0x7FFF) : 0);
    }
}

void parse(int n) {
    uint32_t b = 0x10u * n;
    Channel& c = ch[n];
    uint32_t cnt = reg32(b);
    if ((cnt & 0x80000000u) && !(c.cnt & 0x80000000u)) {
        static const char* const kFmt[4] = {"PCM8", "PCM16", "IMA-ADPCM", "PSG/noise"};
        static const char* const kRep[4] = {"manual", "looped", "one-shot", "repeat mode 3"};
        c.key_on = true;
        nds_once("sound-fmt", (cnt >> 29 & 3) * 4 + (cnt >> 27 & 3), "sound: channel %d starts: %s, %s", n,
                 kFmt[cnt >> 29 & 3], kRep[cnt >> 27 & 3]);
    }
    c.cnt = cnt;
    c.src = reg32(b + 4) & 0x07FFFFFCu;
    c.reload = reg16(b + 8);
    c.loop = 4u * reg16(b + 0xA);
    c.len = 4u * (reg32(b + 0xC) & 0x3FFFFF);
}

void parse_capture(int k) {
    Capture& p = cap[k];
    uint8_t cnt = g_regs[0x108 + k];
    if ((cnt & 0x80) && !(p.cnt & 0x80)) {
        p.timer = p.reload;
        p.pos = 0;
        nds_once("sound-cap", cnt, "sound: capture %d starts, control %02X", k, cnt);
        if (cnt & 1) nds_once("sound-cap-add", k, "sound: capture %d's channel added to the next (not done)", k);
    }
    p.cnt = cnt & 0x8F;
    if (!(p.cnt & 0x80)) p.cnt &= ~1;
    g_regs[0x108 + k] = p.cnt;
    p.dst = reg32(0x110 + 8 * k) & 0x07FFFFFCu;
    p.len = std::max(4u * reg16(0x114 + 8 * k), 4u);
    p.reload = ch[1 + 2 * k].reload;
}

}  // namespace

bool g_sound_keep;

std::vector<int16_t>& sound_samples() { return g_out; }

void sound_sync(uint64_t t) {
    while (g_next <= t) {
        mix_one();
        g_next += kSampleTicks;
    }
}

void sound_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = addr - 0x04000400u;
    if (off + size > sizeof g_regs) return;
    sound_sync(nds_now());
    std::memcpy(&g_regs[off], &v, size);
    if (off < 0x100) {
        int n = (int)(off >> 4);
        parse(n);
        if (n == 1 || n == 3) cap[n / 2].reload = ch[n].reload;    // the capture units run on these timers
    } else if (off < 0x104) {
        g_regs[0x100] &= 0x7F;                      // SOUNDCNT: bits 7 and 14 are not kept
        g_regs[0x101] &= 0xBF;
    } else if (off >= 0x108) {
        for (int k = 0; k < 2; ++k) parse_capture(k);
    }
    io_set16(7, addr & ~1u, reg16(off & ~1u));
}

uint32_t sound_read(uint32_t addr, int size) {
    uint32_t off = addr - 0x04000400u;
    if (off + size > sizeof g_regs) return 0;
    sound_sync(nds_now());
    uint32_t v = 0;
    std::memcpy(&v, &g_regs[off], size);
    return v;
}

// Each running channel's place in its samples, for comparisons with an oracle.
void sound_dump(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (int n = 0; n < 16; ++n)
        if (ch[n].cnt & 0x80000000u) std::fprintf(f, "ch%d pos %d timer %04X\n", n, ch[n].pos, ch[n].timer);
    std::fclose(f);
}

void sound_reset() {
    std::memset(g_regs, 0, sizeof g_regs);
    g_regs[0x105] = 0x02;                           // SOUNDBIAS 0x200, as the boot leaves it
    std::memset(ch, 0, sizeof ch);
    std::memset(cap, 0, sizeof cap);
    g_next = 0;
    g_out.clear();
}
