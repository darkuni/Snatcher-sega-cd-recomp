// HLE replacements for the Sega CD BIOS entry points Snatcher uses.
//   main CPU: $364 (boot security check), $368 (set V-int handler), $28C (return to BIOS)
//   sub CPU:  $5F10 _WAITVSYNC, $5F16 _BURAM, $5F22 _CDBIOS
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "system.h"

static uint32_t rd32(Cpu& c, uint32_t a) { return c.rd32(a); }

// ---------------------------------------------------------------- CD (ROMREADN + CDC)

static const int64_t SUB_HZ = 12500000;
static const int64_t SECTOR_CYCLES = SUB_HZ / 75;          // 1x speed: 75 sectors a second

static void cd_fill(Cpu& c) {
    CdHle& cd = g.cd;
    if (cd.ready || !cd.left || c.cycles < cd.next_ready) return;
    fseek(cd.iso, (long)cd.lba * 2048, SEEK_SET);
    if (fread(cd.buf, 1, 2048, cd.iso) != 2048) memset(cd.buf, 0, 2048);
    cd.cur = cd.lba;
    cd.lba++;
    cd.left--;
    cd.ready = true;
    g.n_sectors++;
    cd.next_ready = c.cycles + (cd.fast ? SECTOR_CYCLES / 16 : SECTOR_CYCLES);
}

static void msf(uint32_t lba, uint8_t out[4]) {
    lba += 150;
    auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    out[0] = bcd(lba / 4500);
    out[1] = bcd((lba / 75) % 60);
    out[2] = bcd(lba % 75);
    out[3] = 1;     // mode 1
}

static void cdbios(Cpu& c) {
    uint32_t fn = c.d[0] & 0xffff;
    CdHle& cd = g.cd;
    c.c = false;
    switch (fn) {
    case 0x0002: cd.audio_track = 0; break;                                   // MSCSTOP
    case 0x0008: case 0x0009: break;                                          // ROMPAUSEON/OFF
    case 0x0010: break;                                                       // DRVINIT
    case 0x0011: case 0x0012: case 0x0013:                                    // MSCPLAY / PLAY1 / PLAYR
        cd.audio_track = c.rd16(c.a[0]);
        fprintf(stderr, "[cd] play track %d (%s)\n", cd.audio_track, fn == 0x12 ? "once" : "repeat");
        break;
    case 0x0017:                                                              // ROMREAD: until stopped
        cd.lba = rd32(c, c.a[0]); cd.left = 0x7fffffff; cd.ready = false;
        cd.next_ready = c.cycles + SECTOR_CYCLES;
        break;
    case 0x0018: cd.lba = rd32(c, c.a[0]); cd.left = 0; cd.ready = false; break;   // ROMSEEK
    case 0x0020:                                                              // ROMREADN
        cd.lba = rd32(c, c.a[0]); cd.left = rd32(c, c.a[0] + 4); cd.ready = false;
        cd.next_ready = c.cycles + (cd.fast ? SECTOR_CYCLES / 16 : SECTOR_CYCLES * 2);
        break;
    case 0x0081:                                                              // CDBSTAT
        g.prg[0x5e80] = 0; g.prg[0x5e81] = 0;                                 // BIOS idle / ready
        c.a[0] = 0x5e80;
        break;
    case 0x0084: break;                                                       // CDBPAUSE
    case 0x0089: cd.left = 0; cd.ready = false; break;                        // CDCSTOP
    case 0x008a: cd_fill(c); c.c = !cd.ready; break;                          // CDCSTAT
    case 0x008b: {                                                            // CDCREAD
        cd_fill(c);
        if (!cd.ready) { c.c = true; break; }
        uint8_t h[4]; msf(cd.cur, h);
        c.d[0] = (h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        c.d[1] = c.d[0];
        // the sector moves to the destination chosen in $FF8004 (DMA ones finish at once)
        if (g.cdc_mode == 5) {
            uint32_t a = (uint32_t)g.dma_addr << 3;
            for (int k = 0; k < 2048; k++) g.prg[(a + k) & 0x7ffff] = cd.buf[k];
            g.cdc_dsr = g.cdc_edt = true;
        } else if (g.cdc_mode == 7) {
            uint32_t a = (uint32_t)g.dma_addr << 3;
            for (int k = 0; k < 2048; k++) g.word[(a + k) & 0x3ffff] = cd.buf[k];
            g.cdc_dsr = g.cdc_edt = true;
        } else if (g.cdc_mode == 4) {
            uint32_t a = (uint32_t)g.dma_addr << 2;
            for (int k = 0; k < 2048; k++) g.pcm[(a + k) & 0xffff] = cd.buf[k];
            g.cdc_dsr = g.cdc_edt = true;
        } else {
            g.cdc_dsr = true;           // sub CPU (CDCTRN) or main CPU ($A12008) reads it
        }
        break;
    }
    case 0x008c: {                                                            // CDCTRN: a0 data, a1 header
        if (!cd.ready) { c.c = true; break; }
        for (int k = 0; k < 2048; k++) c.wr8(c.a[0] + k, cd.buf[k]);
        uint8_t h[4]; msf(cd.cur, h);
        for (int k = 0; k < 4; k++) c.wr8(c.a[1] + k, h[k]);
        c.a[0] += 2048; c.a[1] += 4;
        g.cdc_edt = true;
        break;
    }
    case 0x008d: cd.ready = false; break;                                     // CDCACK
    default:
        fprintf(stderr, "[cd] unhandled CDBIOS %04x\n", fn);
        break;
    }
}

// ---------------------------------------------------------------- backup RAM (files kept in a save file)

struct BramFile { uint8_t flag; uint16_t blocks; std::vector<uint8_t> data; };
static std::map<std::string, BramFile> s_bram;
static bool s_bram_loaded = false;

static std::string bram_path() { return g.game_dir + "/snatcher.brm"; }

static void bram_load() {
    if (s_bram_loaded) return;
    s_bram_loaded = true;
    FILE* f = fopen(bram_path().c_str(), "rb");
    if (!f) return;
    char name[12] = {};
    uint8_t hdr[3];
    while (fread(name, 1, 11, f) == 11 && fread(hdr, 1, 3, f) == 3) {
        BramFile b{hdr[0], (uint16_t)((hdr[1] << 8) | hdr[2]), {}};
        b.data.resize(b.blocks * (b.flag ? 32 : 64));
        if (fread(b.data.data(), 1, b.data.size(), f) != b.data.size()) break;
        s_bram[std::string(name, 11)] = b;
    }
    fclose(f);
}

static void bram_save() {
    FILE* f = fopen(bram_path().c_str(), "wb");
    if (!f) return;
    for (auto& kv : s_bram) {
        uint8_t hdr[3] = {kv.second.flag, (uint8_t)(kv.second.blocks >> 8), (uint8_t)kv.second.blocks};
        fwrite(kv.first.data(), 1, 11, f);
        fwrite(hdr, 1, 3, f);
        fwrite(kv.second.data.data(), 1, kv.second.data.size(), f);
    }
    fclose(f);
}

static std::string bram_name(Cpu& c) {
    std::string s(11, ' ');
    for (int k = 0; k < 11; k++) s[k] = (char)c.rd8(c.a[0] + k);
    return s;
}

static void buram(Cpu& c) {
    bram_load();
    uint32_t fn = c.d[0] & 0xffff;
    c.c = false;
    int used = 0;
    for (auto& kv : s_bram) used += kv.second.blocks;
    switch (fn) {
    case 0:                                                    // BRMINIT
        c.d[0] = 0x7d;                                         // 125 blocks (8 KB internal backup RAM)
        c.d[1] = 0;
        break;
    case 1: c.d[0] = 0x7d - used; c.d[1] = (uint32_t)s_bram.size(); break;   // BRMSTAT
    case 2: {                                                  // BRMSERCH
        auto it = s_bram.find(bram_name(c));
        if (it == s_bram.end()) { c.c = true; break; }
        c.d[0] = it->second.blocks; c.d[1] = it->second.flag;
        break;
    }
    case 3: {                                                  // BRMREAD: a0 name, a1 buffer
        auto it = s_bram.find(bram_name(c));
        if (it == s_bram.end()) { c.c = true; break; }
        for (size_t k = 0; k < it->second.data.size(); k++) c.wr8(c.a[1] + (uint32_t)k, it->second.data[k]);
        c.d[0] = it->second.blocks; c.d[1] = it->second.flag;
        break;
    }
    case 4: {                                                  // BRMWRITE: a0 name+flag+blocks, a1 data
        BramFile b;
        b.flag = (uint8_t)c.rd8(c.a[0] + 11);
        b.blocks = (uint16_t)c.rd16(c.a[0] + 12);
        b.data.resize(b.blocks * (b.flag ? 32 : 64));
        for (size_t k = 0; k < b.data.size(); k++) b.data[k] = (uint8_t)c.rd8(c.a[1] + (uint32_t)k);
        s_bram[bram_name(c)] = b;
        bram_save();
        break;
    }
    case 5: s_bram.erase(bram_name(c)); bram_save(); break;   // BRMDEL
    case 6: s_bram.clear(); bram_save(); break;               // BRMFORMAT
    case 8: break;                                            // BRMVERIFY
    default: fprintf(stderr, "[bram] unhandled BURAM %04x\n", fn); break;
    }
}

// ---------------------------------------------------------------- dispatch

bool bios_call(Cpu& c, uint32_t addr) {
    if (c.id == CPU_MAIN) {
        switch (addr) {
        case 0x000364: break;                                  // boot security check
        case 0x000368: g.main_vint = c.a[1]; break;            // set V-int handler
        case 0x00028c:
            fprintf(stderr, "[main] game returned to the BIOS ($28C) - stopping\n");
            exit(1);
        default: return false;
        }
    } else {
        switch (addr) {
        case 0x005f10: {                                       // _WAITVSYNC
            uint64_t f = g.frame;
            while (g.frame == f) c.sync(0x5f10);
            break;
        }
        case 0x005f16: buram(c); break;
        case 0x005f22: cdbios(c); break;
        default: return false;
        }
    }
    c.ret = c.pop32();
    return true;
}
