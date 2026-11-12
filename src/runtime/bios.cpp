// HLE replacements for the Sega CD BIOS entry points Snatcher uses.
//   main CPU: $364 (boot security check), $368 (set V-int handler), $28C (return to BIOS),
//             $FFFDAE (RAM cartridge backup RAM - reports no cartridge)
//   sub CPU:  $5F10 _WAITVSYNC, $5F16 _BURAM, $5F22 _CDBIOS
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "system.h"
#include "disc.h"

static uint32_t rd32(Cpu& c, uint32_t a) { return c.rd32(a); }

// ---------------------------------------------------------------- CD (ROMREADN + CDC)

static const int64_t SUB_HZ = 12500000;
static const int64_t SECTOR_CYCLES = SUB_HZ / 75;          // 1x speed: 75 sectors a second

static void cd_fill(Cpu& c) {
    CdHle& cd = g.cd;
    if (cd.ready || !cd.left || c.cycles < cd.next_ready) return;
    if (!g_disc.tracks.empty()) g_disc.read_data(cd.lba, cd.buf);
    else {
        fseek(cd.iso, (long)cd.lba * 2048, SEEK_SET);
        if (fread(cd.buf, 1, 2048, cd.iso) != 2048) memset(cd.buf, 0, 2048);
    }
    cd.cur = cd.lba;
    cd.lba++;
    cd.left--;
    cd.ready = true;
    g.n_sectors++;
    cd.next_ready = c.cycles + (cd.fast ? SECTOR_CYCLES / 16 : SECTOR_CYCLES);
}

void CdHle::load_tracks(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[cd] no %s - CD audio disabled\n", path); return; }
    char kind[16];
    Track t;
    while (ntracks < 100 && fscanf(f, "%d %d %d %15s", &t.n, &t.start, &t.len, kind) == 4) {
        t.audio = kind[0] == 'a';
        tracks[ntracks++] = t;
    }
    fclose(f);
}

int CdHle::track_at(uint32_t lba) const {
    for (int k = ntracks - 1; k >= 0; k--)
        if ((int)lba >= tracks[k].start) return tracks[k].n;
    return 1;
}

void CdHle::play(int track, int mode, int64_t now) {
    for (int k = 0; k < ntracks; k++)
        if (tracks[k].n == track) {
            audio_track = track; play_mode = mode; paused = false;
            play_from = pos = tracks[k].start; play_t0 = now;
            return;
        }
    audio_track = 0; play_mode = 0;
}

void CdHle::update(int64_t now) {
    if (!play_mode || paused) return;
    pos = play_from + (uint32_t)((now - play_t0) / SECTOR_CYCLES);
    const Track* t = nullptr;
    for (int k = 0; k < ntracks; k++) if (tracks[k].n == audio_track) t = &tracks[k];
    if (!t) { play_mode = 0; return; }
    uint32_t end = t->start + t->len;
    if (play_mode == 1) {                                   // to end of disc: move through the tracks
        const Track& last = tracks[ntracks - 1];
        if (pos >= (uint32_t)(last.start + last.len)) { play_mode = 0; audio_track = 0; return; }
        audio_track = track_at(pos);
    } else if (pos >= end) {
        if (play_mode == 3) { play_from = t->start; play_t0 = now; pos = play_from; }
        else { play_mode = 0; audio_track = 0; }
    }
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
    static const char* log = getenv("SNATCHER_CDLOG");
    if (log && g.frame >= (uint64_t)atoi(log)) {
        static uint32_t last = ~0u; static int rep = 0;
        if (fn == last && fn >= 0x8a && fn <= 0x8a) { if (++rep % 500 == 0) fprintf(stderr, "[cd] f%llu %02x x%d ready=%d left=%u\n", (unsigned long long)g.frame, fn, rep, cd.ready, cd.left); }
        else { rep = 0; fprintf(stderr, "[cd] f%llu fn %02x a0=%06x [%08x %08x] ready=%d left=%u lba=%u mode=%d\n", (unsigned long long)g.frame, fn, c.a[0], c.rd32(c.a[0]), c.rd32(c.a[0] + 4), cd.ready, cd.left, cd.lba, g.cdc_mode); }
        last = fn;
    }
    c.c = false;
    switch (fn) {
    case 0x0002: cd.play_mode = 0; cd.audio_track = 0; break;                // MSCSTOP
    case 0x0003: cd.update(c.cycles); cd.paused = true; break;               // MSCPAUSEON
    case 0x0004:                                                              // MSCPAUSEOFF
        if (cd.paused) { cd.paused = false; cd.play_from = cd.pos; cd.play_t0 = c.cycles; }
        break;
    case 0x0008: case 0x0009: break;                                          // ROMPAUSEON/OFF
    case 0x0010: break;                                                       // DRVINIT
    case 0x0011: case 0x0012: case 0x0013: {                                  // MSCPLAY / PLAY1 / PLAYR
        int t = c.rd16(c.a[0]);
        cd.play(t, fn == 0x11 ? 1 : fn == 0x12 ? 2 : 3, c.cycles);
        fprintf(stderr, "[cd] play track %d (%s)\n", t, fn == 0x11 ? "to end" : fn == 0x12 ? "once" : "repeat");
        break;
    }
    case 0x0017:                                                              // ROMREAD: until stopped
        cd.lba = rd32(c, c.a[0]); cd.left = 0x7fffffff; cd.ready = false;
        cd.next_ready = c.cycles + SECTOR_CYCLES;
        break;
    case 0x0018: cd.lba = rd32(c, c.a[0]); cd.left = 0; cd.ready = false; break;   // ROMSEEK
    case 0x0020:                                                              // ROMREADN
        cd.lba = rd32(c, c.a[0]); cd.left = rd32(c, c.a[0] + 4); cd.ready = false;
        cd.next_ready = c.cycles + (cd.fast ? SECTOR_CYCLES / 16 : SECTOR_CYCLES * 2);
        break;
    case 0x0081: {                                                            // CDBSTAT
        cd.update(c.cycles);
        uint8_t* t = &g.prg[0x5e80];
        memset(t, 0, 0x20);
        t[0] = cd.play_mode ? (cd.paused ? 0x05 : 0x01) : 0x00;               // drive state: stopped/playing/paused
        uint8_t a[4], r[4];
        msf(cd.pos, a);
        uint32_t tstart = 0;
        for (int k = 0; k < cd.ntracks; k++) if (cd.tracks[k].n == cd.track_at(cd.pos)) tstart = cd.tracks[k].start;
        msf(cd.pos - tstart - 150, r);                                         // relative time (msf adds 150)
        t[8] = a[0]; t[9] = a[1]; t[10] = a[2]; t[11] = (uint8_t)cd.track_at(cd.pos);
        t[12] = r[0]; t[13] = r[1]; t[14] = r[2]; t[15] = 0;
        t[16] = 1; t[17] = cd.ntracks ? (uint8_t)cd.tracks[cd.ntracks - 1].n : 1;
        if (cd.ntracks) {
            uint8_t lo[4]; msf(cd.tracks[cd.ntracks - 1].start + cd.tracks[cd.ntracks - 1].len, lo);
            t[20] = lo[0]; t[21] = lo[1]; t[22] = lo[2];
        }
        c.a[0] = 0x5e80;
        break;
    }
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
            uint32_t a = ((uint32_t)g.dma_addr << 2) & 0xfff;     // into the selected wave RAM bank
            for (int k = 0; k < 2048; k++) pcm_dma_write(a + k, cd.buf[k]);
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
    if (getenv("SNATCHER_BRAMLOG")) {
        char nm[12] = {}; for (int k = 0; k < 11; k++) nm[k] = (char)c.rd8(c.a[0] + k);
        fprintf(stderr, "[bram] f%llu fn %u a0=%06x name '%s' a1=%06x d1=%08x\n", (unsigned long long)g.frame, fn, c.a[0], fn >= 2 && fn <= 5 ? nm : "", c.a[1], c.d[1]);
    }
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
        case 0xfffdae:                                         // backup RAM cartridge calls: none present
            c.c = true;
            c.d[1] = 0;
            break;
        case 0x00028c:
            fprintf(stderr, "[main] game returned to the BIOS ($28C) - stopping\n");
            fflush(stderr);
            _Exit(1);
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
