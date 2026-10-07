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
| pc-moon | Moon (2009) | the cartridge (header, ModuleParams and autoloads, FNT/FAT, NitroROM images nested as archives), the address maps, the BIOS's LZ77, SDAT, the ARM/Thumb decoder |

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
```

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this cartridge? | `rom --info`: header and its CRCs, the secure area's state, both programs, NitroSDK's ModuleParams (autoloads to ITCM/DTCM, static BSS, compression, SDK version), overlay tables, banner (titles, icon) | a `fingerprint`: the SDK's ARM7 components by crc32, the SDK's libraries and middleware (Mobiclip, CRI, ...) by signature |
| 2. Extract | Turn standard formats into standard files | `rom --extract` (files, `sys/` with header, programs, overlays and banner; NitroROM images inside, `--nested`; the SDK's backward LZ undone), `lz` (LZ10, LZ11, RLE), `sdat` (every record, files named by their symbols) | G2D (NCLR/NCGR/NSCR/NCER/NANR) to images, NSBMD/NSBTX, SWAV/STRM to WAV, NARC, NFTR |
| 3. Map code | What does the code do, where? | `arm` (ARMv5TE and Thumb decoder, disassembly with literals resolved and registers named, `--refs`, `--census`), `hw` (both CPUs' address maps and register names) | function discovery for CodeWarrior's ARM/Thumb code, an ARM interpreter as the recompiler's oracle, SDK names by signature |
| 4. Translate | Turn ARM code into C++ | | the recompiler: conditions, interworking, the PC as a register, switch tables, literal pools, processor modes |
| 5. Runtime | Replace the hardware | | memory with VRAM banks, interrupts, timers, DMA, IPC, the cartridge, the BIOS's SWIs; the ARM7 as a coroutine; the 2D engines; the geometry engine and an OpenGL renderer at any resolution; the sound channels; SDL3 |

## Principles

* Pure Python, no dependencies, for layers 1–4; the runtime (layer 5)
  will be C++20 with SDL3 and OpenGL 4.5, built with CMake, Ninja and
  clang, as in wiikit and saturnkit.
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

## Known gaps

* `rom`: compressed overlays are read (the OVT's flag) but none has been
  met yet; the backward LZ is checked on an ARM9 only.
* `lz`: Huffman (types 0x2x) is not done.
* `arm`: should-be-zero fields are checked for MRS, MSR, BX, BLX, CLZ and
  PLD only; cp10/cp11 decode as plain coprocessor instructions (their v5
  meaning); there is no code/data or ARM/Thumb region detection yet, so
  `--census` and `--refs` over a whole program include data read as code.

## Licence

MIT — see [LICENSE](LICENSE). ndskit contains no game data and no
Nintendo code; it reads and replaces, it does not include.

ndskit has been proven on one game so far; its interfaces will change as
the next one asks things of it.
