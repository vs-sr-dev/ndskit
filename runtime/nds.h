// ndskit runtime — the machine: what its parts share. arm.h is the CPUs as
// recompiled code sees them; this is the rest of the DS around them.
//
// Time is counted in ticks of the ARM9's clock (67.03 MHz); the system
// clock that the timers, the display and the ARM7 run on (33.51 MHz) is 2
// ticks. A line is 355 dots of 6 system cycles, a frame 263 lines.
#pragma once
#include "arm.h"
#include <cstdint>
#include <string>
#include <vector>

struct NdsConfig {
    std::string rom;                    // the cartridge
    std::string out = "build/run";      // where the run writes (memories, the save, logs)
    std::string save;                   // the backup memory's file (default: OUT/<rom>.sav)
    uint32_t save_size = 8192;          // its size when the file does not exist yet (the chip's type)
    uint64_t stop_vblanks = 0;          // stop after this many VBlanks (0: never)
    bool trace = false;                 // log the machine's events
    bool dump = true;                   // write the memories when the run stops
    int calls = 0;                      // trace calls, print the last N of each CPU at the end
    uint32_t break_at = 0;              // stop at the first call to this entry (key) of break_cpu
    int break_cpu = 9;
    uint32_t watch_lo = 0, watch_len = 0;   // report the stores to this range
    std::string input;                  // buttons and taps by VBlank (input.cpp)
    int ticks9 = 16;                    // ticks a safe point stands for on the ARM9
    int ticks7 = 24;                    // ... and on the ARM7
    bool window = false;                // a window, in real time, the keyboard and mouse as input
    int scale = 0;                      // the 3D's resolution: 256x192 times this (0: from the window)
    std::vector<uint64_t> shots;        // headless: both screens as PNGs at these VBlanks
    uint64_t shot_every = 0;            // ... and every N VBlanks
};
extern NdsConfig g_cfg;

static const uint64_t kLineTicks = 355 * 6 * 2;
static const uint64_t kFrameTicks = 263 * kLineTicks;
static const uint64_t kHBlankTicks = 256 * 6 * 2;  // into the line, where HBlank starts

// ---- logging ---------------------------------------------------------------------------
void nds_note(const char* fmt, ...);
void nds_trace(const char* fmt, ...);                     // only with g_cfg.trace
[[noreturn]] void nds_fatal(const char* fmt, ...);
void nds_once(const char* what, uint32_t key, const char* fmt, ...);   // the first time only

// ---- the CPUs and time (machine.cpp) ---------------------------------------------------
struct NdsCpu {
    ArmContext c;
    int id;                             // 9 or 7
    uint64_t time;                      // its own clock, in ticks
    bool halted;
    uint32_t ime, ie, iflags;           // IME, IE, IF
};
NdsCpu& nds_cpu(int cpu);
int     nds_running();                  // 9, 7, or 0 between quanta
uint64_t nds_now();                     // the running CPU's time, or the machine's
uint64_t nds_vblanks();

// Events: a callback at a time. One per id; scheduling again moves it.
enum NdsEvent {
    EV_HBLANK, EV_LINE,
    EV_TIMER9, EV_TIMER7 = EV_TIMER9 + 4,
    EV_CARD = EV_TIMER7 + 4,
    EV_COUNT
};
typedef void (*NdsEventFn)(uint64_t when);
void nds_event_set(int ev, NdsEventFn fn);
void nds_schedule(int ev, uint64_t when);
void nds_cancel(int ev);

// Interrupts: IF bits by number (0 VBlank, 1 HBlank, 2 VCount, 3-6 timers,
// 7 RTC, 8-11 DMA, 12 keys, 16 IPC sync, 17 IPC send empty, 18 IPC receive
// not empty, 19 card done, 21 geometry FIFO, 22 lid, 23 SPI).
void nds_irq_raise(int cpu, int bit);
void nds_irq_update(int cpu);           // after IME, IE or IF changed
void nds_halt(ArmContext& c);           // the CPU waits until IE & IF (an interrupt may be taken)
void nds_spend(ArmContext& c, uint64_t ticks);   // time a BIOS call takes
void nds_stop(const char* why);
void nds_count_vblank();
uint32_t nds_irq_marker(int cpu);       // the return address an interrupt pushes in the running thread
void nds_irq_return(ArmContext& c);     // after the BIOS's return from an interrupt: maybe another thread
void nds_cp15_boot();
bool nds_boot(const NdsConfig& cfg);    // the machine as the BIOS leaves it; false if it cannot start
bool nds_run_frame();                   // runs to the next VBlank; false once stopped
[[noreturn]] void nds_finish();         // the report, the memories, the save; exits

// ---- the BIOS (bios.cpp) ----------------------------------------------------------------
void bios_direct_boot();                // memory and registers as melonDS's direct boot leaves them
void bios_swi(ArmContext& c, uint32_t num);
void bios_irq(ArmContext& c);           // take an interrupt (IRQ mode, the user's handler, return)

// ---- the I/O and memory map (mmio.cpp) --------------------------------------------------
void mmio_reset();
uint32_t io_reg(int cpu, uint32_t addr);                // raw storage, 32-bit, aligned
void     io_set(int cpu, uint32_t addr, uint32_t v);    // raw storage
uint16_t io_reg16(int cpu, uint32_t addr);
void     io_set16(int cpu, uint32_t addr, uint16_t v);
void     mmio_set_keys(uint16_t keys, uint16_t ext);    // active low, as KEYINPUT and EXTKEYIN

// ---- timers, DMA, IPC, the maths unit (system.cpp) -------------------------------------
void system_reset();
void timer_write(int cpu, int n, int half, uint16_t v); // half 0: reload, 1: control
uint16_t timer_read(int cpu, int n);
void dma_write(int cpu, int ch);        // its control word changed
void dma_event(int cpu, int mode);      // start mode: 1 VBlank, 2 HBlank (both CPUs' numbering)
void dma_card(int cpu);                 // a word of the card is ready
void ipc_write(int cpu, uint32_t addr, uint32_t v, int size);
uint32_t ipc_read(int cpu, uint32_t addr, int size);
void math_write(uint32_t addr);         // the ARM9's DIV and SQRT registers

// ---- the display (video.cpp) -----------------------------------------------------------
void video_reset();
uint16_t video_vcount();
uint16_t video_dispstat(int cpu);
void video_dispstat_write(int cpu, uint16_t v);
void vram_cnt_write(int bank, uint8_t v);               // VRAMCNT_A..I (0..8)
uint8_t* vram_page(int cpu, uint32_t addr, int which);  // the banks under a VRAM address, nullptr past the last
extern uint8_t g_palette[0x800], g_oam[0x800];

// VRAM as its users see it: areas of 16 KiB pages, each page up to four
// banks' pages (where banks overlap a read ORs them).
enum {
    VRAM_BGA, VRAM_BGB, VRAM_OBJA, VRAM_OBJB, VRAM_LCDC, VRAM_ARM7, VRAM_TEX, VRAM_TEXPAL,
    VRAM_BGEXTA, VRAM_BGEXTB, VRAM_OBJEXTA, VRAM_OBJEXTB,   // the extended palettes
    VRAM_NAREAS
};
struct VramPage { uint8_t* p[4]; int n; };
extern VramPage g_vram_map[VRAM_NAREAS][41];
extern const int g_vram_pages[VRAM_NAREAS];
static inline uint32_t vram_pg(int area, uint32_t off) {     // the page, the area repeated past its end
    uint32_t pg = off >> 14, n = (uint32_t)g_vram_pages[area];
    return pg < n ? pg : pg % n;
}
static inline uint16_t vram_r16(int area, uint32_t off) {
    const VramPage& P = g_vram_map[area][vram_pg(area, off)];
    uint32_t o = off & 0x3FFEu;
    uint16_t v = 0, w;
    for (int k = 0; k < P.n; ++k) { std::memcpy(&w, P.p[k] + o, 2); v |= w; }
    return v;
}
static inline uint8_t vram_r8(int area, uint32_t off) {
    const VramPage& P = g_vram_map[area][vram_pg(area, off)];
    uint32_t o = off & 0x3FFFu;
    uint8_t v = 0;
    for (int k = 0; k < P.n; ++k) v |= P.p[k][o];
    return v;
}
void vram_copy(int area, uint8_t* dst, uint32_t size);   // an area's first `size` bytes, as read

// ---- the 2D engines (gpu2d.cpp) ----------------------------------------------------------
// Each engine draws a line at a time as the line starts, into words the
// compositor (display.cpp) turns into pixels with the 3D picture:
//   word 0: the colour, RGB666 (r bits 0-5, g 6-11, b 12-17); bits 24-25 how
//           it meets the 3D layer: 0 not at all, 1 the 3D is on top (bit 26:
//           blended with this colour, the one under it, by the 3D's alpha),
//           2 this colour is on top, blended with the 3D under it
//   word 1: EVA (bits 0-4) and EVB (8-12) of a blend with the 3D; EVY (16-20)
//           and the 3D's brightness effect (21-22: 2 up, 3 down); the master
//           brightness (24-25: 1 up, 2 down; 26-30 its factor)
struct Gpu2dFrame { uint32_t px[2][192][256][2]; };   // engine A, engine B
extern Gpu2dFrame g_2d;
void gpu2d_reset();
void gpu2d_line(int y);                 // both engines draw line y (0-191)
void gpu2d_vblank();
void gpu2d_written(uint32_t off);       // a write to the ARM9's registers 0x000-0x06F or 0x1000-0x106F
bool gpu2d_top_is_a();                  // POWCNT1 bit 15: engine A on the upper screen
extern bool g_2d_on;                    // the engines draw (headless runs: only for a picture's frame)
// the compositor in software: one pixel's words with the 3D's (RGB666 + alpha in bits 18-22)
uint32_t gpu2d_compose(const uint32_t* w, uint32_t px3d);   // -> 0xRRGGBB

// ---- the geometry engine (gx.cpp) -----------------------------------------------------
void gx_reset();
void gx_write(uint32_t addr, uint32_t v, int size);     // GXFIFO, the command ports, the 3D registers
uint32_t gx_read(uint32_t addr, int size);
void gx_vblank();                       // a SWAP_BUFFERS waiting: the lists go to the rendering engine
void gx_render_start();                 // the rendering engine starts a frame: its job is taken (render3d.h)
void gx_line();                         // the geometry FIFO's interrupt, as each line passes

// ---- the cartridge (cart.cpp) ----------------------------------------------------------
bool cart_open(const std::string& path);
const std::vector<uint8_t>& cart_rom();
uint32_t cart_chip_id();
void cart_write(int cpu, uint32_t addr, uint32_t v, int size);
uint32_t cart_read(int cpu, uint32_t addr, int size);
uint32_t cart_data_word(int cpu);       // 0x04100010
bool cart_word_ready();
void cart_save();

// ---- the ARM7's serial devices: SPI (power, firmware, touch) and the RTC (spi.cpp) -------
void spi_reset();
void spi_write(uint32_t addr, uint32_t v, int size);
uint32_t spi_read(uint32_t addr, int size);
void rtc_write(uint16_t v);
uint16_t rtc_read();
const uint8_t* firmware_user_settings();                // 0x70 bytes, as the firmware holds them

// ---- input from a script (input.cpp) ----------------------------------------------------
void input_parse(const std::string& script);
void input_vblank(uint64_t vblank);     // sets the buttons and the stylus held at this VBlank

// ---- sound (sound.cpp) -----------------------------------------------------------------
// The sound unit runs in virtual time: sound_sync mixes up to a time (each quantum, each register access).
static const uint64_t kSampleTicks = 2048;     // an output sample: 1 024 system cycles (32 728.5 Hz)
void sound_reset();
void sound_write(uint32_t addr, uint32_t v, int size);
uint32_t sound_read(uint32_t addr, int size);
void sound_sync(uint64_t t);
extern bool g_sound_keep;               // keep the output (someone listens): stereo, interleaved
std::vector<int16_t>& sound_samples();  // ... mixed since the listener last cleared it
void sound_dump(const std::string& path);   // the channels' places (a run's end)
