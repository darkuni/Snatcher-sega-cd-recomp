// The Sega CD machine: memory, gate array, buses, HLE BIOS state.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "m68k.h"

enum { CPU_MAIN = 0, CPU_SUB = 1 };

struct MainBus : Bus {
    uint8_t rd8(uint32_t a) override;
    uint16_t rd16(uint32_t a) override;
    void wr8(uint32_t a, uint8_t v) override;
    void wr16(uint32_t a, uint16_t v) override;
};
struct SubBus : Bus {
    uint8_t rd8(uint32_t a) override;
    uint16_t rd16(uint32_t a) override;
    void wr8(uint32_t a, uint8_t v) override;
    void wr16(uint32_t a, uint16_t v) override;
};

// Minimal VDP stand-in until the real one (step 4): registers, status, V-int timing.
struct VdpStub {
    uint8_t reg[32] = {};
    bool pending = false;       // first half of a two-word command
    uint16_t first = 0;
    uint16_t status(int line) const { return 0x3600 | (line >= 224 ? 8 : 0); }
    void ctrl(uint16_t v);
    bool vint_enabled() const { return reg[1] & 0x20; }
    bool hint_enabled() const { return reg[0] & 0x10; }
};

// CD drive / CDC as the BIOS presents it (HLE): ROMREADN feeds sectors at 75/s.
struct CdHle {
    FILE* iso = nullptr;
    uint32_t lba = 0, left = 0;     // pending read
    int64_t next_ready = 0;         // sub-CPU cycle at which the next sector is in the CDC buffer
    bool ready = false;             // a sector waits in the buffer (CDCSTAT)
    bool fast = false;              // fast loading option
    uint32_t cur = 0;               // sector being handed out
    uint8_t buf[2048];
    int audio_track = 0;            // CD-DA track playing (0 = none)
};

struct Scd {
    uint8_t wram[0x10000];          // main work RAM $FF0000
    uint8_t prg[0x80000];           // sub PRG-RAM (main sees 128 KB banks at $020000)
    uint8_t word[0x40000];          // word RAM
    uint8_t pcm[0x10000];           // PCM wave RAM
    uint8_t bram[0x2000];           // backup RAM (BURAM HLE)

    // gate array
    bool sub_reset = true, sub_busreq = true;
    uint8_t prg_bank = 0, prg_protect = 0;
    bool ret = true, dmna = false, mode1m = false;
    uint16_t hint_vec = 0;
    uint8_t flag_main = 0, flag_sub = 0;
    uint16_t cmd[8] = {}, stat[8] = {};
    uint8_t sub_imask = 0;          // $FF8033: enabled sub interrupt levels (bit n = level n)
    bool sub_int2 = false;          // pending level 2 (main -> sub, IFL2)
    uint16_t timer = 0;
    bool vint_pending = false;      // main level 6 (VDP V-blank)
    uint8_t cdc_mode = 0;           // $FF8004 bits 2-0: CDC destination (2 main read, 3 sub read, 4 PCM, 5 PRG-RAM, 7 word RAM)
    bool cdc_dsr = false, cdc_edt = false;   // data set ready / end of data transfer
    uint16_t dma_addr = 0;          // $FF800A
    uint32_t n_vint = 0, n_int2 = 0, n_ifl2 = 0, n_sectors = 0;   // debug counters

    // HLE BIOS state
    uint32_t main_vint = 0;         // level 6 handler given to BIOS $368
    uint32_t sp_entry[4] = {};      // SP module: init, main, int2, user
    CdHle cd;

    VdpStub vdp;
    int line = 0;                   // current scanline (0..261)
    uint64_t frame = 0;

    MainBus mbus;
    SubBus sbus;
    Cpu main, sub;

    bool force_interp = false;      // SNATCHER_INTERP=1: run everything in Musashi (debug)
    std::string game_dir;
    std::vector<uint32_t> misses[2];
    void log_miss(int cpu, uint32_t pc);

    bool boot(const std::string& dir);
};

extern Scd g;

static inline uint16_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static inline void wbe16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

// HLE BIOS (bios.cpp): returns true if addr is a BIOS entry it handled (the call is complete).
bool bios_call(Cpu& c, uint32_t addr);
void cd_tick(int64_t sub_cycles);
