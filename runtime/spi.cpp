// ndskit runtime — the ARM7's serial devices: the SPI bus (the power
// manager, the firmware's flash, the touch screen's controller) and the RTC.
//
// The firmware is made up, as melonDS makes it when it has no dump (the
// oracle's settings): 128 KiB, the user settings twice at its end with their
// CRC (nickname "melonDS", English, birthday 1/1, the touch screen's
// calibration). The RTC keeps a fixed date (2009-04-20, Moon's European
// release: a run is the same run every time), in 24-hour mode.
#include "nds.h"
#include <cstring>

// ---- the firmware ---------------------------------------------------------------------------
static uint8_t g_fw[0x20000];
static const uint32_t kUserSettings = 0x1FE00;

static uint16_t crc16(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) crc = crc & 1 ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    return (uint16_t)crc;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void make_firmware() {
    std::memset(g_fw, 0xFF, sizeof g_fw);
    std::memset(g_fw, 0, 0x200);
    put16(g_fw + 0x20, kUserSettings >> 3);          // where the user settings are, / 8
    g_fw[0x1D] = 0xFF;                               // console type: DS
    // the wireless settings the ARM7 copies to 0x027FFCF4 at boot: melonDS's MAC address
    // (00:09:BF:11:22:33) and enabled channels (1-13)
    static const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};
    std::memcpy(g_fw + 0x36, mac, 6);
    put16(g_fw + 0x3C, 0x3FFE);
    put16(g_fw + 0x2C, 0x0138);                      // the wireless block's length, and its CRC
    put16(g_fw + 0x2A, crc16(0, g_fw + 0x2C, 0x138));
    uint8_t u[0x100];
    std::memset(u, 0, sizeof u);
    put16(u + 0x00, 5);                              // version
    u[0x02] = 0;                                     // favourite colour
    u[0x03] = 1; u[0x04] = 1;                        // birthday: month, day
    const char* name = "melonDS";
    for (int i = 0; name[i]; ++i) put16(u + 0x06 + 2 * i, (uint16_t)name[i]);
    put16(u + 0x1A, 7);
    // touch calibration: ADC (0, 0) at pixel (0, 0), (0xFF0, 0xBF0) at (255, 191)
    put16(u + 0x58, 0); put16(u + 0x5A, 0); u[0x5C] = 0; u[0x5D] = 0;
    put16(u + 0x5E, 0x0FF0); put16(u + 0x60, 0x0BF0); u[0x62] = 0xFF; u[0x63] = 0xBF;
    put16(u + 0x64, 0x0031);                         // English, backlight at its highest
    put16(u + 0x70, 0);                              // update counter
    put16(u + 0x72, crc16(0xFFFF, u, 0x70));
    std::memcpy(g_fw + kUserSettings, u, 0x100);
    std::memcpy(g_fw + kUserSettings + 0x100, u, 0x100);
}

const uint8_t* firmware_user_settings() { return g_fw + kUserSettings; }

// ---- the SPI bus ----------------------------------------------------------------------------
static uint16_t g_spicnt;
static uint8_t g_spidata;
static bool g_sel;                                  // a device is selected (a command is running)
static int g_dev;
static uint32_t g_n;                                // bytes of this command so far
static uint8_t g_cmd;
static uint32_t g_addr;
static uint8_t g_pm[8] = {0x0D, 0x00, 0x01, 0x00, 0x40};   // the power manager's registers
static uint8_t g_fw_status;
static uint16_t g_tsc_val;
static int g_tsc_left;
static bool g_pen;
static uint16_t g_pen_x, g_pen_y;                    // ADC values while the pen is down

static uint8_t power(uint8_t v) {
    if (g_n == 0) { g_cmd = v; return 0; }
    uint8_t reg = g_cmd & 7;
    if (g_cmd & 0x80) return g_pm[reg];
    g_pm[reg] = v;
    return 0;
}

static uint8_t firmware(uint8_t v) {
    if (g_n == 0) { g_cmd = v; g_addr = 0; return 0; }
    switch (g_cmd) {
    case 0x03: case 0x0B:                           // READ, FAST READ (a dummy byte first)
        if (g_n <= 3) {
            g_addr = g_addr << 8 | v;
            if (g_n == 3) nds_trace("firmware: read from %06X", g_addr);
            return 0;
        }
        if (g_cmd == 0x0B && g_n == 4) return 0;
        return g_fw[g_addr++ % sizeof g_fw];
    case 0x05: return g_fw_status;
    case 0x06: g_fw_status |= 2; return 0;
    case 0x04: g_fw_status &= ~2; return 0;
    case 0x9F: { static const uint8_t id[3] = {0x20, 0x40, 0x12}; return g_n <= 3 ? id[g_n - 1] : 0xFF; }
    case 0x0A: case 0x02:                           // page write, page program
        if (g_n <= 3) { g_addr = g_addr << 8 | v; return 0; }
        if (g_fw_status & 2) g_fw[g_addr++ % sizeof g_fw] = g_cmd == 0x02 ? (uint8_t)(g_fw[g_addr % sizeof g_fw] & v) : v;
        return 0;
    }
    nds_once("firmware cmd", g_cmd, "firmware: command %02X not done", g_cmd);
    return 0xFF;
}

static uint8_t touch(uint8_t v) {
    uint8_t out = 0;
    if (g_tsc_left == 2) out = (uint8_t)(g_tsc_val >> 5);
    else if (g_tsc_left == 1) out = (uint8_t)(g_tsc_val << 3);
    if (g_tsc_left) --g_tsc_left;
    if (v & 0x80) {
        int ch = (v >> 4) & 7;
        switch (ch) {
        case 1: g_tsc_val = g_pen ? g_pen_y : 0xFFF; break;   // Y
        case 5: g_tsc_val = g_pen ? g_pen_x : 0; break;       // X
        case 3: g_tsc_val = g_pen ? 0x200 : 0; break;         // Z1
        case 4: g_tsc_val = g_pen ? 0xE00 : 0xFFF; break;     // Z2
        case 0: g_tsc_val = 0x2D4; break;                     // temperature 0
        case 7: g_tsc_val = 0x358; break;                     // temperature 1
        default: g_tsc_val = 0; break;
        }
        if (v & 0x08) g_tsc_val &= 0xFF0;                     // 8-bit mode
        g_tsc_left = 2;
    }
    return out;
}

void spi_write(uint32_t addr, uint32_t v, int size) {
    (void)size;
    if ((addr & 0xFF) == 0xC0) {
        g_spicnt = (uint16_t)(v & 0xCF03);
        if (!(v & 0x8000)) g_sel = false;
        return;
    }
    if (!(g_spicnt & 0x8000)) return;
    int dev = (g_spicnt >> 8) & 3;
    if (!g_sel || dev != g_dev) { g_sel = true; g_dev = dev; g_n = 0; }
    uint8_t b = (uint8_t)v;
    switch (dev) {
    case 0: g_spidata = power(b); break;
    case 1: g_spidata = firmware(b); break;
    case 2: g_spidata = touch(b); break;
    default: g_spidata = 0; break;
    }
    ++g_n;
    if (!(g_spicnt & 0x800)) g_sel = false;
    if (g_spicnt & 0x4000) nds_irq_raise(7, 23);
}

uint32_t spi_read(uint32_t addr, int size) {
    (void)size;
    return (addr & 0xFF) == 0xC0 ? g_spicnt : g_spidata;
}

void spi_set_pen(bool down, int x, int y) {
    g_pen = down;
    g_pen_x = (uint16_t)(x << 4);
    g_pen_y = (uint16_t)(y << 4);
}

// ---- the RTC --------------------------------------------------------------------------------
static uint16_t g_rtc_io;
static uint8_t g_in, g_in_bit, g_in_pos;
static uint8_t g_out[8];
static uint8_t g_out_bit, g_out_pos;
static uint8_t g_rcmd;                              // command number (0-7) and read bit (0x80)
static uint8_t g_stat1 = 0x02, g_stat2, g_alarm1[3], g_alarm2[3], g_adjust, g_free;

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }
static uint8_t bitrev(uint8_t v) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) if (v >> i & 1) r |= (uint8_t)(0x80 >> i);
    return r;
}

static void datetime(uint8_t* o) {
    // seconds of DS time since the run began, onto 2009-04-20 (a Monday) 12:00:00
    uint64_t s = nds_now() / 67027964ull;
    int sec = (int)(s % 60), min = (int)(s / 60 % 60), hour = (int)(12 + s / 3600 % 24) % 24;
    o[0] = bcd(9); o[1] = bcd(4); o[2] = bcd(20); o[3] = 1;
    o[4] = (uint8_t)(bcd(hour) | (hour >= 12 ? 0x40 : 0));
    o[5] = bcd(min); o[6] = bcd(sec);
}

static void rtc_byte(uint8_t b) {
    if (g_in_pos == 0) {
        uint8_t c = (b & 0x0F) == 0x06 ? b : bitrev(b);
        g_rcmd = (uint8_t)(((c >> 4) & 7) | (c & 0x80));
        std::memset(g_out, 0, sizeof g_out);
        if (g_rcmd & 0x80) {
            switch (g_rcmd & 7) {
            case 0: g_out[0] = g_stat1; g_stat1 &= 0x0F; break;   // reading clears the power and interrupt flags
            case 1: std::memcpy(g_out, g_alarm1, 3); break;
            case 2: datetime(g_out); break;
            case 3: g_out[0] = g_adjust; break;
            case 4: g_out[0] = g_stat2; break;
            case 5: std::memcpy(g_out, g_alarm2, 3); break;
            case 6: { uint8_t d[7]; datetime(d); std::memcpy(g_out, d + 4, 3); break; }
            case 7: g_out[0] = g_free; break;
            }
        }
        return;
    }
    int i = g_in_pos - 1;
    switch (g_rcmd & 7) {
    case 0: if (i == 0) g_stat1 = (uint8_t)((g_stat1 & 0xF1) | (b & 0x0E)); break;
    case 1: if (i < 3) g_alarm1[i] = b; break;
    case 3: if (i == 0) g_adjust = b; break;
    case 4: if (i == 0) g_stat2 = b; break;
    case 5: if (i < 3) g_alarm2[i] = b; break;
    case 7: if (i == 0) g_free = b; break;
    default: break;                                 // the date is fixed: writes to it are dropped
    }
}

void rtc_write(uint16_t v) {
    if (v & 4) {
        if (!(g_rtc_io & 4)) {                      // chip select: a new command
            g_in = 0; g_in_bit = 0; g_in_pos = 0; g_out_bit = 0; g_out_pos = 0;
        } else if (!(v & 2)) {                      // clock low: a bit moves
            if (v & 0x10) {
                g_in |= (uint8_t)((v & 1) << g_in_bit);
                if (++g_in_bit == 8) { rtc_byte(g_in); g_in = 0; g_in_bit = 0; ++g_in_pos; }
            } else {
                g_rtc_io = (uint16_t)((g_rtc_io & ~1) | (g_out[g_out_pos] >> g_out_bit & 1));
                if (++g_out_bit == 8) { g_out_bit = 0; if (g_out_pos < 7) ++g_out_pos; }
            }
        }
    }
    g_rtc_io = (v & 0x10) ? v : (uint16_t)((g_rtc_io & 1) | (v & 0xFFFE));
}

uint16_t rtc_read() { return g_rtc_io; }

void spi_reset() {
    make_firmware();
    g_spicnt = 0; g_sel = false; g_tsc_left = 0; g_pen = false;
    g_rtc_io = 0; g_stat1 = 0x02;
}
