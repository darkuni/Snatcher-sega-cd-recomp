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

## Runtime findings
- BIOS calls used: sub $5F10 (_WAITVSYNC), $5F16 (_BURAM), $5F22 (_CDBIOS: 08 12 20 81 84 89 8A 8B 8C 8D);
  main $364 (boot check), $368 (set V-int handler, a1 = $FF06C8), $28C (return to BIOS on error).
- H-int vector ($A12006) = $08E2 -> $FF08E2 (just rte). V-int handler $FF06C8 ends in rte.
- SP copies $7300-$75CF (720 bytes) through word RAM $B7800; IP copies it to $FFE100 and runs it
  (jump table: jmp $FFE290, jmp $FFE140, ...).
- SP's file loader uses the CDC's DMA: $FF8004 = destination (3 sub read via CDCTRN, 5 PRG-RAM,
  7 word RAM), $FF800A = address >> 3, then CDCREAD and polls $FF8004 bit 7 (EDT).
- SP's loader is a coroutine: $6986 pops its caller's return address into $922(a5) and returns
  one level up; $695C resumes it with jmp (a0). Recompiled code handles this by unwinding.
- Timing: the V-int handler clears main flag bit 1, raises IFL2 (sub level 2) and sets bit 1 again
  later; the sub's int2 must see bit 1 clear, so the main CPU yields to the sub the moment it raises
  IFL2 (the real sub answers within microseconds).
- Main program runs inside the V-int handler: $FF06C8 -> state machine at $FF09F0 indexed by $E022
  (0 init, 1 intro/logos with sub-steps in $E04A, ...). $E028 guards re-entry; nested V-ints take the
  short path at $FF0900, which waits for V-blank to end.
- State code polls VDP status bit 3 (V-blank) at $FF2D34. The V-blank flag must rise before the V-int is
  taken (as on hardware) or the poll never sees it - the runtime delays the V-int ~120 cycles.
- SUBCODE installs a level-3 (timer) handler: $D48E written to the BIOS jump table at $5F84, level 3
  enabled with ori #8,$FF8032 - it drives the sound driver ($15106).
- The RAM routine at $FFE100 (from SP) calls $FFFDAE with d0=0 (BRMINIT), a0 work RAM, a1 strings:
  the main BIOS's RAM-cartridge backup RAM call. The runtime reports no cartridge (carry set).
- Intro: licence text -> green scanner line -> Konami logo (512-frame timer, jingle $10203 via $FF19F2)
  -> fade -> CD-DA track 2. The next part waits on the CD audio play position.
- CDBSTAT table ($5E80): +0 status word, high byte = drive state ($00 stopped, $01 playing, $05 paused,
  bit 15 = busy); +8 absolute time, +$C relative time (read while playing). Track 2 (title music) is
  played with MSCPLAY1 and the intro continues when it ends.
- Sound: the sound driver runs in SUBCODE's level-3 timer interrupt ($D48E via $5F84). FM is written by
  the MAIN CPU directly ($A04000/$A04002, ~1000 writes during the Konami jingle) - no Z80 driver.
  PCM wave RAM is filled at boot; channels start in voiced scenes.
- Controls (Options screen): A draws the Blaster, B cancels, C selects/shoots.
