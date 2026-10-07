"""ndskit — game-agnostic building blocks for Nintendo DS reverse engineering and ports.

Each module handles one thing the DS, its BIOS or Nintendo's SDK impose on
every game, independent of any particular title:

    rom      cartridge images (.nds/.srl): header, CRCs, banner, ARM9/ARM7, NitroSDK
             ModuleParams (autoloads, BSS, SDK version), overlays, FNT/FAT, extraction,
             NitroROM images nested as archives, the SDK's backward LZ
    hw       the two CPUs' address maps and I/O register names
    lz       the BIOS's compressions: LZ77 (LZ10), LZ11, RLE
    sdat     NitroSDK sound archives: sequences, banks, wave archives, streams, players
    arm      ARM and Thumb (ARMv5TE / ARMv4T) decoding, disassembly, literal cross-references
    recomp   layer 4: discover (functions in programs without symbols, read where they run)
    ghidra/  ExportFuncs.java: Ghidra's functions, for cross-checking discovery

Layers 1-4 are pure Python 3.8+ with no dependencies. Game-specific knowledge
belongs in the game's own tools/, not here.
"""
