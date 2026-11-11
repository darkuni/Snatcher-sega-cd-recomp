# Snatcher (Sega CD) - native recompilation (work in progress)

A static recompilation of **Snatcher** (Sega CD, NTSC-U, `T-95035`, Konami, 1994) to native code.
Both 68000 CPUs of the Sega CD - the Genesis main CPU and the Sega CD sub CPU - are translated to
C++ by `src/recomp.py`. A runtime that replaces the Genesis / Sega CD hardware (VDP, gate arrays,
graphics ASIC, PCM, FM, CD drive and CD audio) is the next stage. Targets: Windows and Linux / SteamOS.

**No game data is included.** You need your own copy of the game as a disc image (`.cue` + `.bin`).
Put it next to the program (or in a `game` folder) and run it, or pass the `.cue` path.

## Status

- [x] Disc layout and code map (see `NOTES.md`)
- [x] 68000 decoder (`src/m68k.py`), cross-checked against capstone on the game's code and on
      random instruction words
- [x] Recompiler v1 (`src/recomp.py`): function discovery, jump tables, resumable entry points,
      C++ emission; all generated code compiles
- [x] Differential test against the Musashi 68000 interpreter (`tools/difftest`): random
      instruction sequences incl. branches, dbcc, divides, movem, link/unlk - 0 differences over
      26,000 sequences x 6 random starting states
- [x] Runtime: main/sub memory maps, gate array, CDC DMA, HLE BIOS (CD reads, backup RAM, V-int),
      main/sub CPU scheduling and interrupts, Musashi fallback that logs unseen code for the next recompile
- [x] Boots: both CPUs run, the sub CPU loads 91 sectors and hands over to SUBCODE - all recompiled,
      0 addresses interpreted
- [x] VDP: ports, DMA (68k incl. word RAM, fill, copy), planes A/B, window, sprites, priority,
      per-line/cell scrolling - the licence screen, scanner line and Konami logo render correctly
      (Snatcher does not use the Sega CD graphics ASIC)
- [x] CD audio: play/pause/stop with a real play position (the intro is timed to it), music from the
      disc; reads the .cue/.bin directly - no extraction step for players
- [x] RF5C164 PCM chip (voices), sub-CPU timer interrupt (drives the sound driver)
- [x] SDL2 window (4:3, F11 fullscreen), keyboard + controller, 59.92 Hz pacing
- [x] Reaches the title screen and menus
- [x] FM (YM2612 via ymfm), driven from the main CPU; Z80 RAM (the game keeps music data there)
- [ ] Settings menu, save location, longer play-testing
- [x] Windows and Linux CI builds (`.github/workflows/build.yml`)

## Layout

| Path | What |
|---|---|
| `tools/prepare_disc.py` | extracts the data track, boot programs and files from your `.cue/.bin` |
| `src/m68k.py` | 68000 instruction decoder |
| `src/recomp.py` | 68000 -> C++ static recompiler |
| `src/runtime/` | CPU state + instruction semantics (`m68k.h`), image tables |
| `config/images.txt` | code images: CPU, file, load address, entry points |
| `tools/dis68k.py`, `tools/findbase.py`, `tools/check_decoder.py` | dev tools (need `pip install capstone`) |
| `tools/difftest/` | random-instruction differential test against Musashi |

## Building

`src/gen` (the recompiled C++) is committed, so building needs no game data:
```
cmake -S src -B build && cmake --build build      # needs SDL2 (libsdl2-dev)
build/snatcher "Snatcher (Sega CD) (U)-redump.cue"
```
Regenerating `src/gen` (dev):
```
python3 tools/prepare_disc.py "Snatcher (Sega CD) (U)-redump.cue" game
python3 src/recomp.py game config src/gen
build/snatcher game 600          # headless: runs 600 frames, writes misses.txt
SNATCHER_DUMP=60 build/snatcher game 1200   # also writes frame_NNNNN.ppm every 60 frames
```

Debug switches: `SNATCHER_INTERP=1` runs everything in Musashi; `SNATCHER_INTERP_IMG=IP.bin` or
`SNATCHER_INTERP_LO/HI=<hex>` interpret one image / address range; `SNATCHER_TRACE_TOP=1` logs
top-level dispatches; `SNATCHER_PROFILE=1` prints the loops each CPU waits in;
`SNATCHER_INPUT=1260:80,1266:00` scripts the pad (hex: U D L R B C A Start); `SNATCHER_WAV=out.raw` writes
the audio (s16le stereo 44.1 kHz) in headless runs. Addresses in `misses.txt` go into `config/functions.txt`.
