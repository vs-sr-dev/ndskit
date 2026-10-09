// ndskit runtime — the cartridge: the ROM's commands over the .nds file, and
// the backup memory on AUXSPI over a save file.
//
// ROMCTRL's start bit runs the command in CARD_COMMAND at once: its data (a
// block of 0x200 bytes for the main-data read 0xB7, the chip ID for 0xB8)
// is ready word by word at 0x04100010, read by the CPU or by a DMA channel
// in the card's start mode; when the last word is taken the transfer ends
// and, if AUXSPICNT enables it, the card interrupt is raised. A cartridge
// past its secure area is in KEY2 mode; the commands arrive here plain.
//
// The backup memory is an EEPROM (64 Kbit and up: commands with a 2-byte
// address) or a flash chip (3-byte addresses), its type taken from the
// save file's size (8 KiB by default).
#include "nds.h"
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> g_rom;
static uint32_t g_chip_id;
static uint16_t g_auxcnt;
static uint32_t g_romctrl;
static uint8_t g_cmd[8];
static std::vector<uint32_t> g_data;
static size_t g_pos;
static int g_owner = 9;                             // the CPU that started the transfer

const std::vector<uint8_t>& cart_rom() { return g_rom; }
uint32_t cart_chip_id() { return g_chip_id; }

static std::vector<uint8_t> g_save;
static std::string g_save_path;
static bool g_save_dirty;

bool cart_open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_rom.resize((size_t)n);
    size_t got = std::fread(g_rom.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n || n < 0x200) return false;
    uint32_t mb = (uint32_t)((g_rom.size() + 0xFFFFF) >> 20);
    g_chip_id = 0xC2u | ((mb ? mb - 1 : 0) & 0xFF) << 8;   // as melonDS makes it
    // the save
    g_save_path = g_cfg.save;
    if (g_save_path.empty()) {
        size_t s = path.find_last_of("/\\");
        std::string name = path.substr(s == std::string::npos ? 0 : s + 1);
        size_t d = name.find_last_of('.');
        g_save_path = g_cfg.out + "/" + name.substr(0, d) + ".sav";
    }
    g_save.assign(8192, 0xFF);
    if (FILE* s = std::fopen(g_save_path.c_str(), "rb")) {
        std::fseek(s, 0, SEEK_END);
        long m = std::ftell(s);
        std::fseek(s, 0, SEEK_SET);
        if (m > 0) {
            g_save.resize((size_t)m);
            if (std::fread(g_save.data(), 1, (size_t)m, s) != (size_t)m) g_save.assign(8192, 0xFF);
        }
        std::fclose(s);
    }
    return true;
}

void cart_save() {
    if (!g_save_dirty) return;
    if (FILE* s = std::fopen(g_save_path.c_str(), "wb")) {
        std::fwrite(g_save.data(), 1, g_save.size(), s);
        std::fclose(s);
        nds_note("backup memory written to %s", g_save_path.c_str());
    }
}

// ---- ROM commands ---------------------------------------------------------------------------
static void finish() {
    g_romctrl &= ~0x80800000u;
    if (g_auxcnt & 0x4000) nds_irq_raise(g_owner, 19);
}

static void start(int cpu) {
    g_owner = cpu;
    uint32_t bs = (g_romctrl >> 24) & 7;
    uint32_t len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    g_data.assign(len / 4, 0xFFFFFFFFu);
    g_pos = 0;
    uint8_t op = g_cmd[0];
    if (op == 0xB7) {
        uint32_t addr = (uint32_t)g_cmd[1] << 24 | g_cmd[2] << 16 | g_cmd[3] << 8 | g_cmd[4];
        addr &= (uint32_t)(g_rom.size() - 1) | 0;
        if (addr < 0x8000) addr = 0x8000 + (addr & 0x1FF);
        for (uint32_t i = 0; i < len / 4; ++i) {
            uint32_t a = addr + 4 * i, w = 0xFFFFFFFFu;
            if (a + 4 <= g_rom.size()) std::memcpy(&w, &g_rom[a], 4);
            g_data[i] = w;
        }
        nds_trace("card: read %08X, %u bytes", addr, len);
    } else if (op == 0xB8) {
        for (auto& w : g_data) w = g_chip_id;
    } else {
        nds_once("card cmd", op, "card: command %02X %02X%02X%02X%02X %02X%02X%02X not done (0xFF)", op, g_cmd[1],
                 g_cmd[2], g_cmd[3], g_cmd[4], g_cmd[5], g_cmd[6], g_cmd[7]);
    }
    if (g_data.empty()) { finish(); return; }
    g_romctrl |= 0x80800000u;
    dma_card(cpu);
}

bool cart_word_ready() { return g_pos < g_data.size(); }

uint32_t cart_data_word(int cpu) {
    (void)cpu;
    if (g_pos >= g_data.size()) return 0xFFFFFFFFu;
    uint32_t w = g_data[g_pos++];
    ++g_arm_overlay_epoch;                          // memory may change under an overlay's code
    if (g_pos == g_data.size()) finish();
    return w;
}

// ---- the backup memory ------------------------------------------------------------------------
static uint8_t g_bk_cmd, g_bk_status, g_bk_out;
static uint32_t g_bk_addr, g_bk_n;
static bool g_bk_sel;

static int addr_bytes() { return g_save.size() >= 0x40000 ? 3 : g_save.size() > 512 ? 2 : 1; }

static uint8_t backup_xfer(uint8_t v) {
    if (!g_bk_sel) { g_bk_sel = true; g_bk_cmd = v; g_bk_n = 0; g_bk_addr = 0; }
    else ++g_bk_n;
    uint8_t out = 0xFF;
    int ab = addr_bytes();
    uint8_t cmd = ab == 1 ? (g_bk_cmd & 0xF7) : g_bk_cmd;
    switch (cmd) {
    case 0x06: g_bk_status |= 2; break;             // WREN
    case 0x04: g_bk_status &= ~2; break;            // WRDI
    case 0x05: if (g_bk_n) out = g_bk_status; break;   // RDSR
    case 0x01: if (g_bk_n == 1) g_bk_status = (g_bk_status & 3) | (v & 0x0C); break;
    case 0x03: case 0x02: case 0x0A: {               // READ, WRITE (page program, flash page write)
        if (g_bk_n == 0) {
            if (ab == 1 && (g_bk_cmd & 8)) g_bk_addr = 0x100;
            break;
        }
        if (g_bk_n <= (uint32_t)ab) { g_bk_addr = g_bk_addr << 8 | v; if (ab == 1) g_bk_addr |= (g_bk_cmd & 8) << 5; break; }
        uint32_t a = g_bk_addr % g_save.size();
        if (cmd == 0x03) out = g_save[a];
        else if (g_bk_status & 2) { g_save[a] = v; g_save_dirty = true; }
        ++g_bk_addr;
        break;
    }
    case 0x9F: break;                               // ID: none
    default: nds_once("backup cmd", cmd, "backup: command %02X not done", cmd);
    }
    return out;
}

static void backup_deselect() {
    if (g_bk_sel && (g_bk_cmd == 0x02 || g_bk_cmd == 0x0A || ((g_bk_cmd & 0xF7) == 0x02 && addr_bytes() == 1)))
        g_bk_status &= ~2;                          // a write ends: the latch clears
    g_bk_sel = false;
}

// ---- registers --------------------------------------------------------------------------------
void cart_write(int cpu, uint32_t a, uint32_t v, int size) {
    uint32_t off = a & 0xFF;
    if (size == 4) {
        cart_write(cpu, a, v & 0xFFFF, 2);
        cart_write(cpu, a + 2, v >> 16, 2);
        return;
    }
    switch (off) {
    case 0xA0:                                      // the chip is released after a byte sent without hold,
        if ((g_auxcnt & 0x2040) == 0x2040 && !(v & 0x2000)) backup_deselect();   // or when serial mode ends
        g_auxcnt = (uint16_t)v;
        return;
    case 0xA2:
        if (g_auxcnt & 0x2000) {
            g_bk_out = backup_xfer((uint8_t)v);
            nds_trace("backup: %02X -> %02X%s", v & 0xFF, g_bk_out, (g_auxcnt & 0x40) ? "" : " (end)");
            if (!(g_auxcnt & 0x40)) backup_deselect();
        }
        return;
    case 0xA4: g_romctrl = (g_romctrl & 0xFFFF0000u) | v; return;
    case 0xA6:
        g_romctrl = (g_romctrl & 0x0080FFFFu) | ((v & 0xFF7F) << 16) | (g_romctrl & 0x20000000u);
        if (v & 0x8000) start(cpu);
        return;
    }
    if (off >= 0xA8 && off < 0xB0) { g_cmd[off - 0xA8] = (uint8_t)v; g_cmd[off - 0xA8 + 1] = (uint8_t)(v >> 8); return; }
    // the KEY2 seeds (0x1B0-0x1BB): nothing to do with plain commands
}

uint32_t cart_read(int cpu, uint32_t a, int size) {
    (void)cpu; (void)size;
    switch (a & 0xFF) {
    case 0xA0: return g_auxcnt;
    case 0xA2: return g_bk_out;
    case 0xA4: return g_romctrl & 0xFFFF;
    case 0xA6: return g_romctrl >> 16;
    }
    uint32_t off = a & 0xFF;
    if (off >= 0xA8 && off < 0xB0) return g_cmd[off - 0xA8] | g_cmd[off - 0xA8 + 1] << 8;
    return 0;
}
