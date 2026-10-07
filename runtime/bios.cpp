// ndskit runtime — the BIOS: the state a boot leaves, the SWIs, the
// interrupt entry.
//
// The boot is melonDS's direct boot (the oracle's): the programs copied from
// the cartridge to their load addresses, the header and the firmware's user
// settings in main RAM's shared area, the chip ID, the CPUs in system mode
// with the BIOS's stacks. The SWIs are the BIOS's functions done in C++, as
// GBATEK describes them. An interrupt is taken as the BIOS takes it: IRQ
// mode, r0-r3, r12 and the return address pushed on the IRQ stack, the
// handler the program put at the DTCM's end (ARM9) or 0x0380FFFC (ARM7)
// called, the frame popped, `subs pc, lr, #4` (a thread switch in the
// handler changes the frame: nds_irq_return).
#include "nds.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static uint32_t ld32(int cpu, uint32_t a) { return cpu == 9 ? nds9::ld32(a & ~3u) : nds7::ld32(a & ~3u); }
static uint32_t ld16(int cpu, uint32_t a) { return cpu == 9 ? nds9::ld16(a & ~1u) : nds7::ld16(a & ~1u); }
static uint32_t ld8(int cpu, uint32_t a) { return cpu == 9 ? nds9::ld8(a) : nds7::ld8(a); }
static void st32(int cpu, uint32_t a, uint32_t v) { cpu == 9 ? nds9::st32(a & ~3u, v) : nds7::st32(a & ~3u, v); }
static void st16(int cpu, uint32_t a, uint32_t v) { cpu == 9 ? nds9::st16(a & ~1u, v) : nds7::st16(a & ~1u, v); }
static void st8(int cpu, uint32_t a, uint32_t v) { cpu == 9 ? nds9::st8(a, v) : nds7::st8(a, v); }

static uint16_t rd16(const std::vector<uint8_t>& b, size_t o) { return (uint16_t)(b[o] | b[o + 1] << 8); }
static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) { return rd16(b, o) | (uint32_t)rd16(b, o + 2) << 16; }

// ---- the boot -------------------------------------------------------------------------------
static void put32(uint32_t a, uint32_t v) { nds9::st32(a, v); }
static void put16(uint32_t a, uint32_t v) { nds9::st16(a, v); }

void bios_direct_boot() {
    const std::vector<uint8_t>& rom = cart_rom();
    for (uint32_t i = 0; i < 0x170; i += 4) put32(0x027FFE00u + i, rd32(rom, i));
    uint32_t id = cart_chip_id();
    for (uint32_t base : {0x027FF800u, 0x027FFC00u}) {
        put32(base, id);
        put32(base + 4, id);
        put16(base + 8, rd16(rom, 0x15E));          // the header's CRC
        put16(base + 10, rd16(rom, 0x6C));          // the secure area's
    }
    put16(0x027FF850u, 0x5835);                     // the ARM7 BIOS's CRC
    put16(0x027FFC10u, 0x5835);
    put16(0x027FFC30u, 0xFFFF);
    put16(0x027FFC40u, 0x0001);                     // booted from a cartridge
    put32(0x027FF864u, 0);
    put32(0x027FF868u, 0x3FC0u << 3);               // where the firmware keeps the user settings
    const uint8_t* us = firmware_user_settings();
    nds_mem_write(9, 0x027FFC80u, us, 0x70);

    // the programs
    for (int cpu : {9, 7}) {
        uint32_t h = cpu == 9 ? 0x20 : 0x30;
        uint32_t off = rd32(rom, h), ram = rd32(rom, h + 8), size = rd32(rom, h + 12);
        if ((size_t)off + size > rom.size()) nds_fatal("the ARM%d program lies past the cartridge's end", cpu);
        for (uint32_t i = 0; i < size; ++i) st8(cpu, ram + i, rom[off + i]);
        // the CPU: system mode, interrupts masked, the BIOS's stacks
        ArmContext& c = nds_cpu(cpu).c;
        uint32_t entry = rd32(rom, h + 4);
        c.mode = 0x1F;
        c.i = c.f = 1;
        c.r[12] = c.r[14] = entry;
        c.r[13] = cpu == 9 ? 0x03002F7Cu : 0x0380FD80u;
        c.bank[ARM_BANK_IRQ][0] = cpu == 9 ? 0x03003F80u : 0x0380FF80u;
        c.bank[ARM_BANK_SVC][0] = cpu == 9 ? 0x03003FC0u : 0x0380FFC0u;
    }
}

// ---- interrupts -----------------------------------------------------------------------------
static const uint32_t kBiosRet9 = 0xFFFF0290u, kBiosRet7 = 0x00000290u;   // where the BIOS's handler goes on

void bios_irq(ArmContext& c) {
    int cpu = c.cpu;
    uint32_t cpsr = arm_get_cpsr(c);
    uint32_t ret = nds_irq_marker(cpu) + 4;
    arm_set_mode(c, 0x12);
    c.spsr[ARM_BANK_IRQ] = cpsr;
    c.i = 1;
    c.r[14] = ret;
    uint32_t sp = c.r[13] - 24;
    c.r[13] = sp;
    const int saved[6] = {0, 1, 2, 3, 12, 14};
    for (int k = 0; k < 6; ++k) st32(cpu, sp + 4 * k, c.r[saved[k]]);
    c.r[0] = cpu == 9 ? g_dtcm_base + 0x4000u : 0x04000000u;
    uint32_t h = ld32(cpu, c.r[0] - 4);
    uint32_t back = cpu == 9 ? kBiosRet9 : kBiosRet7;
    c.r[14] = back;
    c.pc = 0;
    arm_call(c, cpu == 9 ? arm_iw(h) : h & ~3u);
    if (c.pc != back) nds_fatal("ARM%d: the interrupt handler %08X returned to %08X", cpu, h, c.pc);
    sp = c.r[13];
    for (int k = 0; k < 6; ++k) c.r[saved[k]] = ld32(cpu, sp + 4 * k);
    c.r[13] = sp + 24;
    arm_restore_cpsr(c, c.r[14] - 4);
    nds_irq_return(c);
}

// ---- the SWIs --------------------------------------------------------------------------------
static void intr_wait(ArmContext& c, bool discard, uint32_t mask) {
    int cpu = c.cpu;
    NdsCpu& p = nds_cpu(cpu);
    uint32_t flags = cpu == 9 ? g_dtcm_base + 0x3FF8u : 0x0380FFF8u;
    p.ime = 1;
    nds_irq_update(cpu);
    if (discard) st32(cpu, flags, ld32(cpu, flags) & ~mask);
    for (;;) {
        uint32_t f = ld32(cpu, flags);
        if (f & mask) {
            st32(cpu, flags, f & ~(f & mask));
            return;
        }
        nds_halt(c);
    }
}

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = (uint64_t)std::sqrt((double)v);
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return (uint32_t)r;
}

static void cpu_set(ArmContext& c, bool fast) {
    int cpu = c.cpu;
    uint32_t src = c.r[0], dst = c.r[1], n = c.r[2] & 0x1FFFFF;
    bool fill = c.r[2] >> 24 & 1, words = fast || (c.r[2] >> 26 & 1);
    if (fast) n = (n + 7) & ~7u;
    if (words) {
        src &= ~3u; dst &= ~3u;
        uint32_t v = ld32(cpu, src);
        for (uint32_t i = 0; i < n; ++i) {
            if (!fill) v = ld32(cpu, src + 4 * i);
            st32(cpu, dst + 4 * i, v);
        }
    } else {
        src &= ~1u; dst &= ~1u;
        uint32_t v = ld16(cpu, src);
        for (uint32_t i = 0; i < n; ++i) {
            if (!fill) v = ld16(cpu, src + 2 * i);
            st16(cpu, dst + 2 * i, v);
        }
    }
    nds_spend(c, 8 * (uint64_t)n);
}

// Writes bytes to a destination one at a time, or (VRAM) halfwords.
struct Out {
    int cpu; uint32_t dst; bool half; uint32_t pend = 0; bool odd = false;
    void put(uint8_t b) {
        if (!half) { st8(cpu, dst++, b); return; }
        if (!odd) { pend = b; odd = true; return; }
        st16(cpu, dst, pend | (uint32_t)b << 8);
        dst += 2; odd = false;
    }
    uint8_t back(uint32_t disp, uint32_t written_base) {     // a byte already written, disp back
        uint32_t pos = dst + (odd ? 1 : 0) - disp;
        if (half && odd && pos == dst) return (uint8_t)pend;
        (void)written_base;
        return (uint8_t)ld8(cpu, pos);
    }
};

static void lz77(ArmContext& c, bool half) {
    int cpu = c.cpu;
    uint32_t src = c.r[0];
    uint32_t hdr = ld32(cpu, src);
    uint32_t size = hdr >> 8;
    src += 4;
    Out o{cpu, c.r[1], half};
    uint32_t done = 0;
    while (done < size) {
        uint8_t flags = (uint8_t)ld8(cpu, src++);
        for (int k = 0; k < 8 && done < size; ++k, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b0 = (uint8_t)ld8(cpu, src), b1 = (uint8_t)ld8(cpu, src + 1);
                src += 2;
                uint32_t disp = ((b0 & 0xFu) << 8 | b1) + 1, len = (b0 >> 4) + 3u;
                for (uint32_t i = 0; i < len && done < size; ++i, ++done) o.put(o.back(disp, c.r[1]));
            } else {
                o.put((uint8_t)ld8(cpu, src++));
                ++done;
            }
        }
    }
    nds_spend(c, 4 * (uint64_t)size);
}

static void rl(ArmContext& c, bool half) {
    int cpu = c.cpu;
    uint32_t src = c.r[0];
    uint32_t size = ld32(cpu, src) >> 8;
    src += 4;
    Out o{cpu, c.r[1], half};
    uint32_t done = 0;
    while (done < size) {
        uint8_t f = (uint8_t)ld8(cpu, src++);
        if (f & 0x80) {
            uint8_t b = (uint8_t)ld8(cpu, src++);
            for (uint32_t i = 0; i < (f & 0x7Fu) + 3 && done < size; ++i, ++done) o.put(b);
        } else {
            for (uint32_t i = 0; i < (f & 0x7Fu) + 1 && done < size; ++i, ++done) o.put((uint8_t)ld8(cpu, src++));
        }
    }
    nds_spend(c, 4 * (uint64_t)size);
}

static uint16_t crc16(int cpu, uint32_t crc, uint32_t addr, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= ld8(cpu, addr + i);
        for (int j = 0; j < 8; ++j) crc = crc & 1 ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    return (uint16_t)crc;
}

void bios_swi(ArmContext& c, uint32_t num) {
    int cpu = c.cpu;
    nds_trace("swi %02X (r0 %08X r1 %08X r2 %08X, lr %08X)", num, c.r[0], c.r[1], c.r[2], c.r[14]);
    switch (num) {
    case 0x03:                                      // WaitByLoop
        nds_spend(c, 8 * (uint64_t)(int32_t)std::max<int32_t>((int32_t)c.r[0], 0));
        c.r[0] = 0;
        return;
    case 0x04: intr_wait(c, c.r[0] & 1, c.r[1]); return;    // IntrWait
    case 0x05: intr_wait(c, true, 1); return;               // VBlankIntrWait
    case 0x06: nds_halt(c); return;                         // Halt
    case 0x08:                                      // SoundBias (ARM7)
        if (cpu == 7) { sound_write(0x04000504u, c.r[0] ? 0x200 : 0, 2); return; }
        break;
    case 0x09: {                                    // Div
        int32_t n = (int32_t)c.r[0], d = (int32_t)c.r[1];
        if (d == 0) nds_fatal("swi Div by zero (lr %08X)", c.r[14]);
        int32_t q = (n == INT32_MIN && d == -1) ? n : n / d, r = (n == INT32_MIN && d == -1) ? 0 : n % d;
        c.r[0] = (uint32_t)q; c.r[1] = (uint32_t)r; c.r[3] = (uint32_t)(q < 0 ? -q : q);
        nds_spend(c, 80);
        return;
    }
    case 0x0B: cpu_set(c, false); return;           // CpuSet
    case 0x0C: cpu_set(c, true); return;            // CpuFastSet
    case 0x0D: c.r[0] = isqrt64(c.r[0]); nds_spend(c, 80); return;   // Sqrt
    case 0x0E: c.r[0] = crc16(cpu, c.r[0] & 0xFFFF, c.r[1], c.r[2]); return;  // GetCRC16
    case 0x0F: c.r[0] = 0; return;                  // IsDebugger
    case 0x11: lz77(c, false); return;              // LZ77 to WRAM (bytes)
    case 0x12: if (cpu == 7) { lz77(c, true); return; } break;   // the ARM7's LZ77 to VRAM; the ARM9's takes callbacks
    case 0x14: rl(c, false); return;                // RL to WRAM
    case 0x15: if (cpu == 7) { rl(c, true); return; } break;
    // The ARM7 BIOS's sound tables, worked out from their definitions (an
    // inference: the BIOS's own values may round differently; phase 6 checks)
    case 0x1A:                                      // GetSineTable: a quarter wave, 64 halfwords
        if (cpu == 7) { c.r[0] = (uint32_t)std::lround(std::sin((c.r[0] & 63) * 3.14159265358979323846 / 128) * 32768.0) & 0xFFFF; return; }
        break;
    case 0x1B:                                      // GetPitchTable: 2^(i/768) - 1 in 16.16, 768 halfwords
        if (cpu == 7) { c.r[0] = (uint32_t)std::lround((std::pow(2.0, (c.r[0] % 768) / 768.0) - 1) * 65536.0) & 0xFFFF; return; }
        break;
    case 0x1C: {                                    // GetVolumeTable: 724 bytes, -72.3 dB to 0 in tenths,
        if (cpu != 7) break;                        // scaled by the divider the SDK pairs with each range
        int i = (int)(c.r[0] % 724), db = i - 723;
        double mul = db < -240 ? 16 : db < -120 ? 4 : db < -60 ? 2 : 1;
        c.r[0] = (uint32_t)std::min(127L, std::lround(127.0 * std::pow(10.0, db / 200.0) * mul));
        return;
    }
    }
    nds_fatal("ARM%d: swi %02X not done (r0 %08X r1 %08X r2 %08X, lr %08X)", cpu, num, c.r[0], c.r[1], c.r[2], c.r[14]);
}
