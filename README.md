# ndskit

A game-agnostic toolkit for Nintendo DS reverse engineering and native PC
ports: cartridge images, the SDK's file and sound formats, the two ARM
CPUs, the hardware's address maps, and (as it grows) a static recompiler
from ARM and Thumb code to C++ and a runtime that replaces the DS's
hardware under the recompiled game.

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
| pc-moon | Moon (2009) | the cartridge (header, ModuleParams and autoloads of both CPUs, FNT/FAT, NitroROM images nested as archives), the address maps, the BIOS's LZ77, SDAT, the ARM/Thumb decoder, function discovery for CodeWarrior's ARM code (switches, veneers, hand-written dispatch, vtables of small methods, context saves) with the programs read where they run, an ARM interpreter, the recompiler to C++ and its self-test, the runtime's core (both CPUs in time, the SDK's threads as coroutines, interrupts, the BIOS, the I/O, timers, DMA, IPC, the cartridge and its backup memory, SPI, the RTC), melonDS savestates as the oracle; the picture (the 2D engines in software, the geometry engine, an OpenGL renderer at any scale, a window with the keyboard and the mouse) |

## Using it

ndskit is born inside its first port (`pc-moon/ndskit/`); it will move to
its own repository and be taken by each port as a git submodule at
`ndskit/`, so that `python -m ndskit.…` works from the port's root.

```sh
python -m ndskit.rom GAME.nds --info                 # header, CRCs, programs, autoloads, SDK, banner
python -m ndskit.rom GAME.nds --list                 # the file system, each file's kind
python -m ndskit.rom GAME.nds --extract build/extract --nested   # files, sys/, archives inside
python -m ndskit.lz --scan build/extract             # BIOS-compressed files
python -m ndskit.lz FILE --out FILE.dec
python -m ndskit.sdat SOUND.sdat --list              # sequences, banks, wave archives, streams
python -m ndskit.sdat SOUND.sdat --extract build/sdat
python -m ndskit.arm GAME.nds --offset 4000 --size SIZE --base 02000000 --at 02000800
python -m ndskit.arm FILE --base 02000000 --refs 04000000:04002000   # who loads a register's address
python -m ndskit.hw 04000400 04000540                # names, as the ARM9 sees them
python -m ndskit.hw --arm7 04000400                  # and the ARM7
python -m ndskit.recomp.discover GAME.nds --cpu 9 --report --out funcs9.tsv   # every function, ARM or Thumb
python -m ndskit.recomp.discover GAME.nds --cpu 7                              # the ARM7, where its autoloads run
python -m ndskit.armemu GAME.nds --cpu 9 --call 0201234C --regs r0=7,r1=100   # run one function of the game
python -m ndskit.recomp GAME.nds --out build/recomp [--optest] [--copy 9:DEST=SRC:LEN]   # both programs to C++, CMake
python -m ndskit.recomp.selftest GAME.nds --cpu 9 --auto --out arm9.txt        # vectors of the functions that run alone
cmake -S build/recomp -B build/recomp/b -G Ninja -DCMAKE_CXX_COMPILER=clang++ && ninja -C build/recomp/b
build/recomp/b/selftest build/recomp/selftest/*.txt arm9.txt                    # the recompiled code against them
build/recomp/b/ndsrun GAME.nds --window [--scale 4]                              # the game in a window, in real time
build/recomp/b/ndsrun GAME.nds --out build/run --vblanks 600 [--input 300:START,400:tap:128:96]   # the machine, headless
build/recomp/b/ndsrun GAME.nds --out build/run --vblanks 600 --shots 300,600     # both screens as PNGs at those VBlanks
build/recomp/b/ndsrun GAME.nds --vblanks 600 --break 0211860C --calls 200       # stop at a function, the last calls
build/recomp/b/ndsrun GAME.nds --vblanks 600 --watch 022AA568:4                 # who stores there
python -m ndskit.melonds STATE.ml1 --compare build/run                          # its memories against melonDS's
```

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this cartridge? | `rom --info`: header and its CRCs, the secure area's state, both programs, NitroSDK's ModuleParams (autoloads to ITCM/DTCM, static BSS, compression, SDK version), overlay tables, banner (titles, icon) | a `fingerprint`: the SDK's ARM7 components by crc32, the SDK's libraries and middleware (Mobiclip, CRI, ...) by signature |
| 2. Extract | Turn standard formats into standard files | `rom --extract` (files, `sys/` with header, programs, overlays and banner; NitroROM images inside, `--nested`; the SDK's backward LZ undone), `lz` (LZ10, LZ11, RLE), `sdat` (every record, files named by their symbols) | G2D (NCLR/NCGR/NSCR/NCER/NANR) to images, NSBMD/NSBTX, SWAV/STRM to WAV, NARC, NFTR |
| 3. Map code | What does the code do, where? | `arm` (ARMv5TE and Thumb decoder, disassembly with literals resolved and registers named, `--refs`, `--census`), `hw` (both CPUs' address maps and register names), `armemu` (an interpreter for both CPUs: the recompiler's reference, and a way to run one of the game's functions), `ghidra/ExportFuncs.java` | SDK names by signature |
| 4. Translate | Turn ARM code into C++ | `recomp.discover` (recursive descent over ARM and Thumb with the mode carried through interworking; the program read in segments where it runs, its autoloads included; constants through straight lines for veneers and calls through registers; CodeWarrior's `addls pc` switches; hand-written jumps through tables of offsets, followed through registers; returns set up in lr; pointer, boundary and prologue seeds to a fixed point; what each write to pc is), `recomp.emit` and `python -m ndskit.recomp` (one C++ function per entry, one module per program: conditions, flags stored only where read, the PC as a value, calls through registers dispatched by address and mode, switches, interworking, modes, exception returns), `recomp.selftest` (vectors from the interpreter: every instruction form, the game's functions that run alone) | Thumb switches, halfword-offset tables |
| 5. Runtime | Replace the hardware | `runtime/` (C++20): the CPU context, the memories, the modes, the dispatch, the self-test; the machine (`ndsrun`): both CPUs in quanta of virtual time, each guest thread on its own coroutine (the SDK's context switches followed), interrupts as the BIOS takes them, the BIOS's SWIs, a direct boot as melonDS does it, the I/O of both CPUs, VRAM through its banks, timers, DMA, IPC, the maths unit, the cartridge's commands and its backup memory, SPI (power, firmware, touch), the RTC, the sound registers; a call trace, breaks and a store watch; the 2D engines in software a line at a time, the geometry engine in the DS's fixed point, the rendering engine on OpenGL 4.5 at any scale (the texture memory read in the shader), a compositor, a window (SDL3) with the keyboard and the mouse, headless pictures. `melonds`: savestates read, memories compared | the sound channels and the mixer; the 3D's anti-aliasing, edge marking, shadow polygons; display capture |

## Principles

* Pure Python, no dependencies, for layers 1–4; the runtime (layer 5)
  is C++20, built with CMake, Ninja and clang, with SDL3 and OpenGL
  4.5, as in wiikit and saturnkit.
* Every claim is checked on a real cartridge before it goes in.
* Game knowledge stays out.
* Every change is checked on every port before it goes in.

## Checks behind each module

| Module | Checked by |
|---|---|
| `rom` | Moon (Europe): header and logo CRCs correct, the secure area recognised as decrypted, ModuleParams found through the footer (SDK 4.2, two autoloads, uncompressed), 2 593 files extracted, every FAT entry named; its five language archives read as NitroROM images (287 files each, CRCs correct, their ARM7 byte-identical to the game's). A 2010 cartridge (SDK 5.4) with a compressed ARM9 and 366 overlays: the backward LZ undone to exactly the end of the autoload list (0x4B970 bytes to 0x7BF78), every one of its 13 545 FAT entries a named file (13 179) or an overlay (366). A 2008 cartridge (SDK 4.1, Japanese banner): 3 overlays at one address, 6 378 files |
| `arm` | every Thumb halfword (65 536, with BL/BLX pairs: 67 584 cases) and every word of Moon's ARM9 and ARM7 (386 110) plus 1 000 000 random words, against capstone 5: no disagreement on an ARMv5TE encoding; the differences are instructions the ARM946E-S does not have (ARMv6/v7, Thumb-2, VFP; arm.py says undefined) and should-be-zero fields capstone rejects inconsistently. 206 600 branch targets and literal addresses equal to capstone's |
| `lz` | Moon: 251 scene and 252 collision files, every one decoded to its stated size (479 LZ10, 24 LZ11), the stream ending within the file |
| `sdat` | Moon's sound archive: 43 sequences, 1 sequence archive with 67 named sequences, 31 banks, 1 wave archive, 1 player, every one named by its symbol and its file in range |
| `hw` | register names from GBATEK; those seen in Moon's code noted as they are met |
| `rom` (ARM7 ModuleParams) | found by its shape (the autoload list ending at the program's end) on three cartridges of SDK 4.1, 4.2 and 5.4: two autoloads each, to the ARM7's WRAM and to 0x027E0000 |
| `recomp.discover` | Moon's ARM9 against Ghidra 12: 4 663 of Ghidra's 4 680 functions covered (4 648 as entries, 15 inside another's code), the 17 left hand-written decoder routines reached through register-based tables; 765 more found. Its ARM7: 535 of 536. 0 conflicts (no descent meets data or an undefined instruction) on Moon and on two other cartridges, a Thumb-heavy one included; of Moon's transfers through registers that are not calls, 5 are left unresolved, each one read |
| `armemu` | against Unicorn's ARM946 (QEMU), a scratch script: 12 993 random ARM instructions, 12 490 Thumb ones and 1 960 branches, calls and loads into pc, every register, flag and byte of memory compared: no difference, outside unaligned accesses (QEMU does them as an ARMv6; the DS's CPUs rotate or align, as armemu does, after melonDS) and what the architecture leaves open. The SDK's divisions in Moon on known answers |
| `recomp` (emit, selftest, runtime) | Moon's ARM9 (5 413 functions, 305 574 instructions) and ARM7 (933, 37 664) compile with clang 22 and link; the instruction test of both CPUs (7 292 functions, 57 749 vectors) and 1 512 of Moon's functions that run alone (23 806 vectors) give the same registers, flags and memory as armemu: 81 555 vectors, no difference. A wrong carry planted in the emitter gives 91 failures |
| `runtime` (the machine), `melonds` | Moon boots, plays its movies, reads its files and its save, and with a script of 11 presses goes through its menus into its first room, deterministically (two runs, the same memories). Against melonDS at the same frame of the SDK's VBlank count: after 464 frames with no input and the same save, 74 bytes of main RAM differ (counters that follow time, the RTC's date), the ITCM and the shared WRAM not at all; after the same presses, 3 642 frames, in the same room with the camera's same orientation |
| `runtime` (the picture, session 5) | Moon's logos, movies, menus, title and first room (1 103 polygons a frame, fog) against melonDS's pictures at the same frames: the title's 3D within 496 of 38 400 pixels, the menu's lower screen identical but for a pulsing border, the first room apart at polygon edges (anti-aliasing) and where the player stands; in a window in real time |
| `recomp.discover` (session 4) | Moon's ARM9: all 4 680 of Ghidra's functions covered (6 580 functions, 166 switches); every entry of session 3 kept. 0 conflicts on the three cartridges, each with more functions found (a 2010 one 2 214 to 2 319, a 2008 one 3 090 to 3 201) |

## Known gaps

* `rom`: compressed overlays are read (the OVT's flag) but none has been
  met yet; the backward LZ is checked on an ARM9 only.
* `lz`: Huffman (types 0x2x) is not done.
* `recomp.discover`: CodeWarrior's Thumb switches and a newer SDK ARM7's
  halfword-offset tables are not recognised yet; code a program copies
  elsewhere at run time is not found by itself (the runtime names the
  block when it meets a call into it; `--copy` gives it to the
  recompiler).
* `recomp`: no hooks yet; jumps through registers that are neither calls
  nor returns go to `arm_jump` at run time (an entry is called, anything
  else is taken for a return); a C++ exception's landing (a jump into a
  catch block up the stack) is not unwound to yet.
* `runtime`: no sound yet (the mixer is the next phase); the 3D's
  anti-aliasing, edge marking, shadow polygons, the translucent polygon
  ID rule, the equal depth test, the clear image and 1-dot polygons are
  not drawn, nor are display capture, mosaic and main memory display;
  time is a fixed cost per safe point, not cycles;
  the shared WRAM stays the ARM7's (WRAMCNT = 3); the BIOS's sound tables
  are worked out from their definitions, not read; the backup memory is
  an EEPROM of the save file's size (64 Kbit by default) or a flash chip;
  the firmware is made up (melonDS's defaults) and its wireless block is
  left empty; DMA's display-start and main-memory-display modes are not
  done; the RTC keeps a fixed date.
* `armemu`: no interrupts, timers or cycles; CP15 is a store of values.
* `arm`: should-be-zero fields are checked for MRS, MSR, BX, BLX, CLZ and
  PLD only; cp10/cp11 decode as plain coprocessor instructions (their v5
  meaning); there is no code/data or ARM/Thumb region detection yet, so
  `--census` and `--refs` over a whole program include data read as code.

## Licence

MIT — see [LICENSE](LICENSE). ndskit contains no game data and no
Nintendo code; it reads and replaces, it does not include.

ndskit has been proven on one game so far; its interfaces will change as
the next one asks things of it.
