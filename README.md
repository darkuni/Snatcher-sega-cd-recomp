# Snatcher (Sega CD) - native recompilation (work in progress)

A static recompilation of **Snatcher** (Sega CD, NTSC-U, `T-95035`, Konami, 1994) to native code.
Both 68000 CPUs of the Sega CD - the Genesis main CPU and the Sega CD sub CPU - are translated to
C++ by `src/recomp.py`. A runtime that replaces the Genesis / Sega CD hardware (VDP, gate arrays,
graphics ASIC, PCM, FM, CD drive and CD audio) is the next stage. Targets: Windows and Linux / SteamOS.

**No game data is included.** You need your own copy of the game as a disc image (`.cue` + `.bin`).

## Status

- [x] Disc layout and code map (see `NOTES.md`)
- [x] 68000 decoder (`src/m68k.py`), cross-checked against capstone on the game's code and on
      random instruction words
- [x] Recompiler v1 (`src/recomp.py`): function discovery, jump tables, resumable entry points,
      C++ emission; all generated code compiles
- [x] Differential test against the Musashi 68000 interpreter (`tools/difftest`): random
      instruction sequences incl. branches, dbcc, divides, movem, link/unlk - 0 differences over
      26,000 sequences x 6 random starting states
- [ ] Runtime: memory maps, main/sub CPU scheduling, interrupts, BIOS calls
- [ ] Graphics (VDP + graphics ASIC), PCM + FM + CD audio, input, settings menu
- [ ] Windows and Linux builds

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

## Building the generated code (dev)

```
python3 tools/prepare_disc.py "Snatcher (Sega CD) (U)-redump.cue" game
python3 src/recomp.py game config src/gen
```
