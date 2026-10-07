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
int  nds_main(const NdsConfig& cfg);    // runs until a stop; does not return

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
void gx_write(uint32_t addr, uint32_t v, int size);     // the geometry engine's ports
uint32_t gx_read(uint32_t addr, int size);

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
void sound_write(uint32_t addr, uint32_t v, int size);
uint32_t sound_read(uint32_t addr, int size);
