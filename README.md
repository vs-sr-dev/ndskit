# ndskit

A game-agnostic toolkit for Nintendo DS reverse engineering and native PC
ports: cartridge images, the SDK's file and sound formats, the two ARM
CPUs, the hardware's address maps, a static recompiler from ARM and Thumb
code to C++, and a runtime that replaces the DS's hardware under the
recompiled game: both CPUs, the 2D and 3D engines (the 3D on OpenGL at any
resolution), the sound unit, a window.

The same idea as [wiikit](https://github.com/vs-sr-dev/wiikit) and
[saturnkit](https://github.com/vs-sr-dev/saturnkit), for the DS. Each
DS game has its own engine and formats, but a large part of every port
is the *same* work: the same cartridge layout and file system, the same
two CPUs, the same 2D and 3D engines and VRAM banks, the same sound
channels, and very often the same Nintendo library (NitroSDK) and the
same ARM7 program, which Nintendo shipped to developers ready-made.
ndskit collects that shared part. It grows inside the ports: each piece
is written because a game needed it, then kept free of that game's
knowledge. Game formats and game fixes live in the ports.

## Ports built on it

| Port | Game | What it asked of ndskit |
|---|---|---|
| [pc-moon](https://github.com/vs-sr-dev/pc-moon) | Moon (2009) | everything a first game asks: the cartridge (header, ModuleParams and autoloads of both CPUs, FNT/FAT, NitroROM images nested as archives), the address maps, the BIOS's LZ77, SDAT, the ARM/Thumb decoder, function discovery for CodeWarrior's ARM code (switches, veneers, hand-written dispatch, vtables of small methods, context saves) with the programs read where they run, an ARM interpreter, the recompiler to C++ and its self-test, code a program copies to the ITCM (`--copy`), the runtime's core (both CPUs in virtual time, the SDK's threads as coroutines, interrupts, the BIOS, the I/O, timers, DMA, IPC, the cartridge and its EEPROM, SPI, the RTC), melonDS savestates as the oracle; the picture (the 2D engines in software, the geometry engine in fixed point, an OpenGL renderer at any scale, fog, a window with the keyboard and the mouse as the stylus); the sound (the 16 channels, capture, the mixer, the FIFO's read-ahead, the audio device, WAV files) |
| [pc-nostalgia](https://github.com/vs-sr-dev/pc-nostalgia) | Nostalgia (2009) | overlays all the way: discovery with one loaded and across them (calls, veneers and vtables reaching another slot's overlay), a module per overlay in the recompiler, the runtime's choice of the overlay resident in each area (its code's crc32, sampled words, the first instruction of each dispatched call), the self-test through that dispatch, `melonds --overlays`; discovery of C++'s tiny methods (pointers to member functions, tables of unknown methods, a function filling a gap, a veneer's literal pool as a function's end); display capture; shadow polygons; a 512 Kbit EEPROM (`--save-size`) and the save kept on a fault; `sdat --waves` (ADPCM, PSG and noise banks); the kit split out into its own repository |

Each port says where it stands in a `.recomp.json` at its root
([spec](https://recomp.fyi/spec)): the game, its system,
`"toolchain": "ndskit"` and a status, which trackers such as
[recomp.board](https://recomp.fyi) read. A new port adds one, and changes
its status in the same commit as its README's.

## Using it

Each port takes ndskit as a git submodule at `ndskit/` (`git clone
--recursive`, or `git submodule update --init` after a clone), so that
`python -m ndskit.…` works from the port's root. To build and run:
Python 3, clang, CMake, Ninja, SDL3 (on Windows, MSYS2's
`mingw-w64-x86_64` packages) and a GPU with OpenGL 4.5.

```sh
python -m ndskit.rom GAME.nds --info                 # header, CRCs, programs, autoloads, overlays, SDK, banner
python -m ndskit.rom GAME.nds --list                 # the file system, each file's kind
python -m ndskit.rom GAME.nds --extract build/extract --nested   # files, sys/, archives inside
python -m ndskit.lz --scan build/extract             # BIOS-compressed files
python -m ndskit.lz FILE --out FILE.dec
python -m ndskit.sdat SOUND.sdat --list              # sequences, banks, wave archives, streams
python -m ndskit.sdat SOUND.sdat --waves             # wave formats and rates; PCM, PSG and noise instruments
python -m ndskit.sdat SOUND.sdat --extract build/sdat
python -m ndskit.arm GAME.nds --offset 4000 --size SIZE --base 02000000 --at 02000800
python -m ndskit.arm FILE --base 02000000 --refs 04000000:04002000   # who loads a register's address
python -m ndskit.hw 04000400 04000540                # names, as the ARM9 sees them
python -m ndskit.hw --arm7 04000400                  # and the ARM7
python -m ndskit.recomp.discover GAME.nds --cpu 9 --report --out funcs9.tsv   # every function, ARM or Thumb
python -m ndskit.recomp.discover GAME.nds --cpu 7                              # the ARM7, where its autoloads run
python -m ndskit.recomp.discover GAME.nds --cpu 9 --overlay all --out "f_{id}.tsv"   # each overlay with the program
python -m ndskit.armemu GAME.nds --cpu 9 --call 0201234C --regs r0=7,r1=100   # run one function of the game
python -m ndskit.recomp GAME.nds --out build/recomp [--optest] [--copy 9:DEST=SRC:LEN]   # programs and overlays to C++
python -m ndskit.recomp.selftest GAME.nds --cpu 9 --auto --out arm9.txt        # vectors of the functions that run alone
python -m ndskit.recomp.selftest GAME.nds --cpu 9 --overlay 0 --auto --out ov0.txt   # an overlay's, with it loaded
cmake -S build/recomp -B build/recomp/b -G Ninja -DCMAKE_CXX_COMPILER=clang++ && ninja -C build/recomp/b
build/recomp/b/selftest build/recomp/selftest/*.txt arm9.txt                    # the recompiled code against them
build/recomp/b/ndsrun GAME.nds --window [--scale 4] [--mute]                     # the game in a window, in real time, heard
build/recomp/b/ndsrun GAME.nds --window --save-size 65536                       # a new cartridge with a 512 Kbit EEPROM
build/recomp/b/ndsrun GAME.nds --out build/run --vblanks 600 --wav build/run/a.wav  # its sound as a WAV file
build/recomp/b/ndsrun GAME.nds --out build/run --vblanks 600 [--input 300:START,400:tap:128:96]   # the machine, headless
build/recomp/b/ndsrun GAME.nds --out build/run --vblanks 600 --shots 300,600     # both screens as PNGs at those VBlanks
build/recomp/b/ndsrun GAME.nds --vblanks 600 --break 0211860C --calls 200       # stop at a function, the last calls
build/recomp/b/ndsrun GAME.nds --vblanks 600 --watch 022AA568:4                 # who stores there
python -m ndskit.melonds STATE.ml1 --compare build/run                          # its memories and sound channels against melonDS's
python -m ndskit.melonds STATE.ml1 --info                                       # a state's sections, VBlank count, channels
python -m ndskit.melonds STATE.ml1 --overlays GAME.nds                          # which overlay each load address holds
```

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this cartridge? | `rom --info`: header and its CRCs, the secure area's state, both programs, NitroSDK's ModuleParams (autoloads to ITCM/DTCM, static BSS, compression, SDK version), overlay tables, banner (titles, icon) | a `fingerprint`: the SDK's ARM7 components by crc32, the SDK's libraries and middleware (Mobiclip, CRI, ...) by signature |
| 2. Extract | Turn standard formats into standard files | `rom --extract` (files, `sys/` with header, programs, overlays and banner; NitroROM images inside, `--nested`; the SDK's backward LZ undone), `lz` (LZ10, LZ11, RLE), `sdat` (every record, files named by their symbols; the waves' formats and the banks' instrument kinds) | G2D (NCLR/NCGR/NSCR/NCER/NANR) to images, NSBMD/NSBTX, SWAV/STRM to WAV, NARC, NFTR |
| 3. Map code | What does the code do, where? | `arm` (ARMv5TE and Thumb decoder, disassembly with literals resolved and registers named, `--refs`, `--census`), `hw` (both CPUs' address maps and register names), `armemu` (an interpreter for both CPUs: the recompiler's reference, and a way to run one of the game's functions), `ghidra/ExportFuncs.java` | SDK names by signature (`FS_StartOverlay`, ...) |
| 4. Translate | Turn ARM code into C++ | `recomp.discover` (recursive descent over ARM and Thumb with the mode carried through interworking; the program read in segments where it runs, its autoloads included; constants through straight lines for veneers and calls through registers; CodeWarrior's `addls pc` switches; hand-written jumps through tables of offsets, followed through registers; returns set up in lr; pointer, boundary and prologue seeds to a fixed point, C++'s pointers to member functions and tables of tiny methods among them; what each write to pc is; an overlay loaded with the program, calls into it from outside and from other overlays taken only where they walk clean), `recomp.emit` and `python -m ndskit.recomp` (one C++ function per entry, one module per program and one per overlay, an overlay's calling its program's functions directly: conditions, flags stored only where read, the PC as a value, calls through registers dispatched by address and mode, switches, interworking, modes, exception returns), `recomp.selftest` (vectors from the interpreter: every instruction form, the game's functions that run alone, an overlay's with it loaded) | Thumb switches, halfword-offset tables; overlays a game compresses itself |
| 5. Runtime | Replace the hardware | `runtime/` (C++20): the CPU context, the memories, the modes, the dispatch, the self-test; overlays: an address in an overlay's load area looked up in the module of the overlay resident there, recognised by its code's crc32, checked again by sampled words when the cartridge has delivered data and by the first instruction of each dispatched call; the machine (`ndsrun`): both CPUs in quanta of virtual time, each guest thread on its own coroutine (the SDK's context switches followed), interrupts as the BIOS takes them, the BIOS's SWIs, a direct boot as melonDS does it, the I/O of both CPUs, VRAM through its banks, timers, DMA, IPC, the maths unit, the cartridge's commands and its backup memory (EEPROM or flash, kept on a fault), SPI (power, firmware, touch), the RTC; a call trace, breaks and a store watch; the 2D engines in software a line at a time, display capture, the geometry engine in the DS's fixed point, the rendering engine on OpenGL 4.5 at any scale (the texture memory read in the shader, fog, shadow polygons through the stencil), a compositor, a window (SDL3) with the keyboard and the mouse, headless pictures; the sound unit (16 channels, capture, the mixer) in virtual time, an SDL3 audio stream, WAV files. `melonds`: savestates read, memories and sound channels compared, overlays named | the 3D's anti-aliasing, edge marking, the shadows' polygon ID rule; display capture at the 3D's resolution; the cartridge's time; hooks for a PC version (the camera, the screens' layout) |

## Principles

* Pure Python, no dependencies, for layers 1–4; the runtime (layer 5)
  is C++20, built with CMake, Ninja and clang, with SDL3 and OpenGL
  4.5, as in wiikit and saturnkit.
* Every claim is checked on a real cartridge before it goes in.
* Game knowledge stays out.
* Every change is checked on every port before it goes in, and function
  discovery on two more cartridges (a 2010 one with a compressed ARM9
  and 366 overlays, a 2008 one mostly in Thumb).

## Checks behind each module

| Module | Checked by |
|---|---|
| `rom` | Moon (Europe): header and logo CRCs correct, the secure area recognised as decrypted, ModuleParams found through the footer (SDK 4.2, two autoloads, uncompressed), 2 593 files extracted, every FAT entry named; its five language archives read as NitroROM images (287 files each, CRCs correct, their ARM7 byte-identical to the game's). Nostalgia (USA): 104 files, 14 ARM9 overlays in two slots. A 2010 cartridge (SDK 5.4) with a compressed ARM9 and 366 overlays: the backward LZ undone to exactly the end of the autoload list (0x4B970 bytes to 0x7BF78), every one of its 13 545 FAT entries a named file (13 179) or an overlay (366). A 2008 cartridge (SDK 4.1, Japanese banner): 3 overlays at one address, 6 378 files |
| `rom` (ARM7 ModuleParams) | found by its shape (the autoload list ending at the program's end) on four cartridges of SDK 4.1, 4.2 and 5.4: two autoloads each, to the ARM7's WRAM and to 0x027E0000 |
| `arm` | every Thumb halfword (65 536, with BL/BLX pairs: 67 584 cases) and every word of Moon's ARM9 and ARM7 (386 110) plus 1 000 000 random words, against capstone 5: no disagreement on an ARMv5TE encoding; the differences are instructions the ARM946E-S does not have (ARMv6/v7, Thumb-2, VFP; arm.py says undefined) and should-be-zero fields capstone rejects inconsistently. 206 600 branch targets and literal addresses equal to capstone's |
| `lz` | Moon: 251 scene and 252 collision files, every one decoded to its stated size (479 LZ10, 24 LZ11), the stream ending within the file. A sample of each kind of Nostalgia's `.lz` members inside its own archives |
| `sdat` | Moon's sound archive: 43 sequences, 1 sequence archive with 67 named sequences, 31 banks, 1 wave archive, 1 player, every one named by its symbol and its file in range. `--waves` on four cartridges' five archives: Moon's 102 PCM8 and 2 PCM16 waves, 95 PCM programs and 10 key splits (115 instruments, as its sound unit plays them); Nostalgia's 971 waves all IMA-ADPCM, its effects' 138 PSG and 19 noise programs; the others' ADPCM, PCM16, PSG, noise and drum sets read through, every offset in range |
| `hw` | register names from GBATEK; those seen in the games' code noted as they are met |
| `armemu` | against Unicorn's ARM946 (QEMU), a scratch script: 12 993 random ARM instructions, 12 490 Thumb ones and 1 960 branches, calls and loads into pc, every register, flag and byte of memory compared: no difference, outside unaligned accesses (QEMU does them as an ARMv6; the DS's CPUs rotate or align, as armemu does, after melonDS) and what the architecture leaves open. The SDK's divisions in Moon on known answers |
| `recomp.discover` | Moon's ARM9 against Ghidra 12: all 4 680 of Ghidra's functions covered (6 613 functions); its ARM7: 535 of 536. Nostalgia's ARM9 with its overlays: 4 090 of Ghidra's 4 101 in the program, the 11 left a cluster of dead code; each of its 14 overlays with 0 conflicts, the program's 117 direct calls into them. 0 conflicts (no descent meets data or an undefined instruction) on the four cartridges, both CPUs; every rule added since checked on all four, no function lost or split. Each of the stops met playing Nostalgia (an empty handler in a table of pointers to member functions, methods returning a type's id, functions reached only from another slot's overlay, veneers and inherited methods in vtables, a prologue after a veneer's literal pool) found by a rule that holds on the other cartridges |
| `recomp` (emit, selftest, runtime) | Moon's ARM9 and ARM7 compile with clang 22 and link; the instruction test of both CPUs (7 292 functions, 57 749 vectors) and 1 512 of Moon's functions that run alone (23 806 vectors) give the same registers, flags and memory as armemu: 81 555 vectors, no difference. A wrong carry planted in the emitter gives 91 failures. Nostalgia's program, 13 overlays and ARM7 compile without a warning; 115 393 vectors with no failure, 23 172 of them three overlays' through the overlay dispatch |
| `runtime` (the machine), `melonds` | Moon boots, plays its movies, reads its files and its save, and with a script of 11 presses goes through its menus into its first room, deterministically (two runs, the same memories). Against melonDS at the same frame of the SDK's VBlank count: after 464 frames with no input and the same save, 74 bytes of main RAM differ (counters that follow time, the RTC's date), the ITCM and the shared WRAM not at all; after the same presses, 3 642 frames, in the same room with the camera's same orientation. Nostalgia from a new cartridge through its title, opening and first dungeon: 2 112 bytes of main RAM differ at frame 8 361 (most of them the sound sequencer's state), its 512 Kbit EEPROM formatted as melonDS's (4 bytes apart) |
| `runtime` (overlays) | Nostalgia played in the window through its first dungeon, three battles and its prologue (20 068 frames), the first area changing hands between its field's and its battles' overlays at each battle, the second area holding the battle's; a run with the cartridge's epoch removed finds the same changes by the other two checks |
| `runtime` (the picture) | Moon's logos, movies, menus, title and first room (1 103 polygons a frame, fog) against melonDS's pictures at the same frames: the title's 3D within 496 of 38 400 pixels, the menu's lower screen identical but for a pulsing border, the first room apart at polygon edges (anti-aliasing) and where the player stands; in a window in real time. Nostalgia's title and first dungeon (with its shadow polygons) against melonDS's pictures; its pre-battle transition (a display capture) played by the user, nothing to report |
| `runtime` (the sound) | Moon's music, effects and movie sound in the window and as WAV files; against melonDS's savestates at five frames with no input, the same channels with the same registers (format, volume, divider, pan, source, timer, loop, length). The BIOS's sine, pitch and volume tables equal to the BIOS's own, all 1 556 values. Nostalgia's IMA-ADPCM music and effects on all 16 channels, by the user's ear |

## Known gaps

* `rom`: compressed overlays are read (the OVT's flag) but none has been
  met yet; the backward LZ is checked on an ARM9 only.
* `lz`: Huffman (types 0x2x) is not done.
* `recomp.discover`: overlays that a game compresses itself (the overlay
  table's flag clear, the 2010 cartridge's) are read as stored;
  CodeWarrior's Thumb switches and a newer SDK ARM7's halfword-offset
  tables are not recognised yet; code a program copies elsewhere at run
  time is not found by itself (the runtime names the block when it meets
  a call into it; `--copy` gives it to the recompiler). A function
  reached only through a pointer built at run time is found when a game
  stops at it: the runtime names it (`call to ..., not an entry`).
* `recomp`: no hooks yet; jumps through registers that are neither calls
  nor returns go to `arm_jump` at run time (an entry is called, anything
  else is taken for a return); a C++ exception's landing (a jump into a
  catch block up the stack) is not unwound to yet.
* `runtime`: the sound's PSG, noise and capture are unchecked against
  melonDS (Nostalgia's PSG and noise heard, not compared); the cartridge
  answers at once (the DS takes time to read); the 3D's anti-aliasing,
  edge marking, the shadows' polygon ID rule, the translucent polygon ID
  rule, the equal depth test, the clear image and 1-dot polygons are not
  drawn, nor are mosaic and main memory display (as a display mode or a
  capture's source); display capture reads the 3D at the DS's resolution;
  time is a fixed cost per safe point, not cycles (Nostalgia runs
  headless at 15 times the DS, Moon at 210); the shared WRAM stays the
  ARM7's (WRAMCNT = 3); the backup memory's type follows the save file's
  size (8 KiB, an EEPROM of 64 Kbit, for a new one unless `--save-size`);
  the firmware is made up (melonDS's defaults) and its wireless block is
  left empty; DMA's display-start and main-memory-display modes are not
  done; the RTC keeps a fixed date.
* `armemu`: no interrupts, timers or cycles; CP15 is a store of values.
* `arm`: should-be-zero fields are checked for MRS, MSR, BX, BLX, CLZ and
  PLD only; cp10/cp11 decode as plain coprocessor instructions (their v5
  meaning); there is no code/data or ARM/Thumb region detection yet, so
  `--census` and `--refs` over a whole program include data read as code.

## History

ndskit grew inside pc-moon from its first session to its sixth and was
split out with its history (`git subtree split`) when the second port,
pc-nostalgia, began. The commit messages of that period describe Moon's
sessions; the changes in them are ndskit's.

## Licence

MIT — see [LICENSE](LICENSE). ndskit contains no game data and no
Nintendo code; it reads and replaces, it does not include.
