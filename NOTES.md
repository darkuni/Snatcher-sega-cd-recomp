# Snatcher (Sega CD) - reverse engineering notes

Disc: `Snatcher (Sega CD) (U)-redump.bin/.cue`, 625,192,176 bytes. Track 01 MODE1/2352 data
(56,014 sectors, 114.7 MB); tracks 02-21 are CD-DA music.

## Disc header
- `SEGADISCSYSTEM`, volume `SEGAIPMENU`, system `KONAMI R002`, serial `GM T-95035 -00`, `(C)T-95 1994.OCT`.
- Header I/O and region fields read "J", but the IP starts by skipping a 0x584-byte security block,
  the size of the US one - the disc is the US release.
- IP (main CPU boot) at disc 0x200-0x6800 -> $FF0000. SP (sub CPU boot) at 0x6800, 0x1800 bytes -> $6000.

## Files (ISO9660 root, file numbers = order of SP's name table at $62FA)
| # | File | Load | What |
|---|---|---|---|
| 0-52 | DATA_A0 ... DATA_Y16 | $28000 (sub) | resource packs. Header: longs pointing into $28000, first = name string. 2-long header = data pack; 3-long header (D0, D1, D2, Y16) = code overlay, entry = 3rd long. Packs are LZSS-compressed (decompressor: IP $FF1994, SUBCODE $F346). +4 = table of command lists run by SUBCODE $F68E |
| 53 | FMWR_1 | $80000 (word RAM) | FM music/sfx data, starts with an offset table |
| 54 | PCMDRMDT | | PCM drum samples (header carries $3E80 = 16000, probably the rate) |
| 55 | PCMLD_01 | | 85 MB streamed voice/sfx bank, same header style |
| 56 | PCMLT_01 | $50000 | PCM tables |
| 57-95 | SP00-SP38 | | bytecode scene scripts (all start with $30, readable labels like "ACT1 SHOOTING"); interpreter not located yet |
| 96 | SUBCODE | $D400 | sub CPU services (CD, PCM, graphics, scene sequencer) |

## Code
- **IP** ($FF0000, main CPU): display side - VDP ($C00000/4), main gate array ($A12000-$A1200F),
  small state machines (e.g. $FFE022 dispatch at $FF09F0).
- **SP** ($6000, sub CPU): header "MAIN A014", entries at +$20 (init $602E, main $60A8, int2 $60BE,
  user $629C). File service dispatcher at $6910 (d0 = function: 9 find, 6/2 load ...). Loader $62AC:
  d1 = file number, a1 = destination. Boot loads SUBCODE to $D400 and jumps to $D47C.
- **SUBCODE** ($D400, sub CPU, confirmed: 183 of 193 absolute call targets land on function starts):
  23-entry jmp table at $D404. File loader $D7EA/$D7FA (own name table at $D832). Scene sequencer:
  word command list at $9940, index $9B40, 36 commands dispatched at $FFDA (0x0D = play music via the
  sound driver jump table at $150F4, 0x11 = load file to address). Music sequencer table at $F924
  (48 commands, bytes >= $80).
- **DATA_D2** ($28000): save/load menu overlay (backup RAM "SNATCHER" saves).

## Recompiler (src/recomp.py) - first run
| Image | Functions | Instructions covered |
|---|---|---|
| IP.bin | 50 | 1371 |
| SP.bin | 11 | 207 |
| SUBCODE.BIN | 223 | 6949 |
| DATA_D2 | 14 | 757 |
| DATA_D0 / D1 / Y16 | 1 each | 36 / 36 / 31 |
IP coverage is low because its interrupt handlers and callbacks are installed at run time; those get
found by logging misses in the runtime and adding them to `config/functions.txt`.
