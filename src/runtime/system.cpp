// Sega CD memory maps and gate array (main side $A12000, sub side $FF8000).
#include "system.h"
#include <cstring>
#include "disc.h"

Scd g;
void yield_to_sub();
uint32_t g_fm_writes = 0, g_psg_writes = 0;

void Scd::log_miss(int cpu, uint32_t pc) {
    for (uint32_t a : misses[cpu]) if (a == pc) return;
    misses[cpu].push_back(pc);
    Cpu& c = cpu ? sub : main;
    fprintf(stderr, "[%s] no recompiled code at %06x (interpreting)  a7=%06x [%08x %08x %08x %08x] expect top %06x\n",
            cpu ? "sub" : "main", pc, c.a[7], c.rd32(c.a[7]), c.rd32(c.a[7] + 4), c.rd32(c.a[7] + 8), c.rd32(c.a[7] + 12),
            c.nexpect > 0 && c.nexpect <= 512 ? c.expect[c.nexpect - 1] : 0);
}

// ---------------------------------------------------------------- main CPU

static uint16_t main_ga_rd(uint32_t a) {
    switch (a & 0x3e) {
    case 0x00: return (g.sub_int2 ? 0x0100 : 0) | (g.sub_busreq ? 2 : 0) | (g.sub_reset ? 0 : 1);
    case 0x02: return (g.prg_protect << 8) | (g.prg_bank << 6) | (g.mode1m ? 4 : 0) | (g.dmna ? 2 : 0) | (g.ret ? 1 : 0);
    case 0x06: return g.hint_vec;
    case 0x0e: return (g.flag_main << 8) | g.flag_sub;
    }
    if ((a & 0x3f) >= 0x10 && (a & 0x3f) < 0x20) return g.cmd[((a & 0x3f) - 0x10) >> 1];
    if ((a & 0x3f) >= 0x20 && (a & 0x3f) < 0x30) return g.stat[((a & 0x3f) - 0x20) >> 1];
    return 0;
}

static void main_ga_wr8(uint32_t a, uint8_t v) {
    switch (a & 0x3f) {
    case 0x00:                                  // IFL2: interrupt the sub CPU (level 2)
        if (v & 1) { g.sub_int2 = true; g.n_ifl2++; yield_to_sub(); }
        return;
    case 0x01:
        g.sub_reset = !(v & 1);
        g.sub_busreq = v & 2;
        return;
    case 0x02: g.prg_protect = v; return;
    case 0x03:
        if (getenv("SNATCHER_WRLOG")) fprintf(stderr, "main a12003=%02x f%llu line %d\n", v, (unsigned long long)g.frame, g.line);
        g.prg_bank = (v >> 6) & 3;
        if (v & 2) { g.dmna = true; if (!g.mode1m) g.ret = false; g.n_dmna++; }   // give 2M word RAM to the sub CPU
        return;
    case 0x06: g.hint_vec = (g.hint_vec & 0xff) | (v << 8); return;
    case 0x07: g.hint_vec = (g.hint_vec & 0xff00) | v; return;
    case 0x0e: g.flag_main = v; return;
    case 0x0f: g.flag_main = v; return;         // byte write to the low half also lands in the main flag
    }
    uint32_t r = a & 0x3f;
    if (r >= 0x10 && r < 0x20) {
        uint16_t& w = g.cmd[(r - 0x10) >> 1];
        w = (r & 1) ? (w & 0xff00) | v : (w & 0xff) | (v << 8);
    }
}

uint8_t MainBus::rd8(uint32_t a) {
    if (a >= 0xff0000) return g.wram[a & 0xffff];
    if (a >= 0x200000 && a < 0x240000) return g.word[a & 0x3ffff];
    if (a >= 0x020000 && a < 0x040000) return g.prg[(g.prg_bank << 17) | (a & 0x1ffff)];
    if (a >= 0xa12000 && a < 0xa12040) { uint16_t w = main_ga_rd(a); return (a & 1) ? (uint8_t)w : w >> 8; }
    if (a >= 0xc00000 && a < 0xc00020) { uint16_t w = rd16(a & ~1); return (a & 1) ? (uint8_t)w : w >> 8; }
    if (a >= 0xa00000 && a < 0xa04000) return g.z80ram[a & 0x1fff];
    if (a >= 0xa04000 && a < 0xa04004) return fm_read(a);
    if (a == 0xa10001) return 0x80;             // overseas, NTSC, expansion (Sega CD) present
    if (a == 0xa10003) {                        // 3-button pad, port 1 (buttons active low)
        uint8_t p = g.pad, th = g.pad_th & 0x40;
        if (getenv("SNATCHER_PADLOG") && g.frame >= 1298 && g.frame <= 1304)
            fprintf(stderr, "pad read f%llu th=%d ctrl=%02x pad=%02x\n", (unsigned long long)g.frame, th ? 1 : 0, g.pad_ctrl, p);
        if (th) return 0x40 | (~((p & 0x0f) | ((p >> 4 & 1) << 4) | ((p >> 5 & 1) << 5)) & 0x3f);   // C B R L D U
        return (~((p & 0x03) | ((p >> 6 & 1) << 4) | ((p >> 7 & 1) << 5)) & 0x33);               // St A 0 0 D U
    }
    if (a == 0xa10005) return 0x7f;             // port 2: nothing connected
    if (a == 0xa10009) return g.pad_ctrl;
    return 0;
}

uint16_t MainBus::rd16(uint32_t a) {
    if (a >= 0xff0000) return be16(&g.wram[a & 0xffff]);
    if (a >= 0x200000 && a < 0x240000) return be16(&g.word[a & 0x3ffff]);
    if (a >= 0x020000 && a < 0x040000) return be16(&g.prg[(g.prg_bank << 17) | (a & 0x1ffff)]);
    if (a >= 0xa12000 && a < 0xa12040) return main_ga_rd(a);
    if (a >= 0xc00000 && a < 0xc00020) {
        switch (a & 0x1c) {
        case 0x00: return g.vdp.data_read();
        case 0x04: { uint16_t s = g.vdp.status(g.line); g.vdp.pending = false; return s; }
        case 0x08: return ((g.line > 0xea ? g.line - 6 : g.line) & 0xff) << 8 | ((g.main.cycles & 0x3ff) >> 2);
        }
        return 0xffff;
    }
    if (a == 0xa11100) return 0;                // Z80 bus granted
    return (rd8(a) << 8) | rd8(a + 1);
}

void MainBus::wr8(uint32_t a, uint8_t v) {
    if (a >= 0xff0000) { g.wram[a & 0xffff] = v; return; }
    if (a >= 0x200000 && a < 0x240000) { g.word[a & 0x3ffff] = v; return; }
    if (a >= 0x020000 && a < 0x040000) { g.prg[(g.prg_bank << 17) | (a & 0x1ffff)] = v; return; }
    if (a >= 0xa12000 && a < 0xa12040) { main_ga_wr8(a, v); return; }
    if (a >= 0xa00000 && a < 0xa04000) { g.z80ram[a & 0x1fff] = v; return; }
    if (a >= 0xa04000 && a < 0xa04004) { g_fm_writes++; fm_write(a, v); return; }
    if (a == 0xc00011) { g_psg_writes++; return; }
    if (a == 0xa10003) { if (g.pad_ctrl & 0x40) g.pad_th = v & 0x40; return; }
    if (a == 0xa10009) { g.pad_ctrl = v; return; }
    if (a >= 0xc00000 && a < 0xc00020) { wr16(a & ~1u, (uint16_t)(v | (v << 8))); return; }   // byte -> both halves
}

void MainBus::wr16(uint32_t a, uint16_t v) {
    if (a >= 0xff0000) { wbe16(&g.wram[a & 0xffff], v); return; }
    if (a >= 0x200000 && a < 0x240000) { wbe16(&g.word[a & 0x3ffff], v); return; }
    if (a >= 0xc00000 && a < 0xc00020) {
        if ((a & 0x1c) == 0x00) g.vdp.data_write(v);
        else if ((a & 0x1c) == 0x04) g.vdp.ctrl_write(v);
        return;
    }
    if (a >= 0xa12000 && a < 0xa12040) {
        uint32_t r = a & 0x3f;
        if (r == 0x00) { main_ga_wr8(a, v >> 8); main_ga_wr8(a + 1, (uint8_t)v); return; }
        if (r == 0x06) { g.hint_vec = v; return; }
        if (r == 0x0e) { g.flag_main = v >> 8; return; }
        if (r >= 0x10 && r < 0x20) { g.cmd[(r - 0x10) >> 1] = v; return; }
        main_ga_wr8(a, v >> 8);
        main_ga_wr8(a + 1, (uint8_t)v);
        return;
    }
    wr8(a, v >> 8);
    wr8(a + 1, (uint8_t)v);
}

// ---------------------------------------------------------------- sub CPU

static uint16_t sub_ga_rd(uint32_t a) {
    uint32_t r = a & 0x1ff;
    switch (r & ~1u) {
    case 0x000: return 0x0001;                  // RES0: not resetting
    case 0x004: return (g.cdc_edt ? 0x8000 : 0) | (g.cdc_dsr ? 0x4000 : 0) | (g.cdc_mode << 8);
    case 0x00a: return g.dma_addr;
    case 0x002: return (g.prg_protect << 8) | (g.mode1m ? 4 : 0) | (g.dmna ? 2 : 0) | (g.ret ? 1 : 0);
    case 0x00e: return (g.flag_main << 8) | g.flag_sub;
    case 0x030: return g.timer;
    case 0x032: return g.sub_imask;
    }
    if (r >= 0x10 && r < 0x20) return g.cmd[(r - 0x10) >> 1];
    if (r >= 0x20 && r < 0x30) return g.stat[(r - 0x20) >> 1];
    return 0;
}

static void sub_ga_wr8(uint32_t a, uint8_t v) {
    uint32_t r = a & 0x1ff;
    switch (r) {
    case 0x003:
        if (getenv("SNATCHER_WRLOG")) fprintf(stderr, "sub  ff8003=%02x f%llu line %d (loop %06x)\n", v, (unsigned long long)g.frame, g.line, g.sub.dbg_pc);
        g.mode1m = v & 4;
        if (v & 1) { g.ret = true; g.dmna = false; g.n_ret++; }    // hand 2M word RAM back to the main CPU
        return;
    case 0x004: g.cdc_mode = v & 7; g.cdc_dsr = g.cdc_edt = false; return;
    case 0x00e: case 0x00f: g.flag_sub = v; return;
    case 0x031: g.timer = v; g.timer_due = g.sub.cycles + (int64_t)(v + 1) * 384; return;   // 30.72 us per count
    case 0x033: g.sub_imask = v; return;
    }
    if (r >= 0x20 && r < 0x30) {
        uint16_t& w = g.stat[(r - 0x20) >> 1];
        w = (r & 1) ? (w & 0xff00) | v : (w & 0xff) | (v << 8);
    }
}

uint8_t SubBus::rd8(uint32_t a) {
    if (a < 0x80000) return g.prg[a];
    if (a < 0xc0000) return g.word[a & 0x3ffff];
    if (a >= 0xfe0000 && a < 0xff0000) return (a & 1) ? g.bram[(a >> 1) & 0x1fff] : 0xff;
    if (a >= 0xff0000 && a < 0xff8000) return (a & 1) ? pcm_read(a) : 0;
    if (a >= 0xff8000) { uint16_t w = sub_ga_rd(a); return (a & 1) ? (uint8_t)w : w >> 8; }
    return 0;
}

uint16_t SubBus::rd16(uint32_t a) {
    if (a < 0x80000) return be16(&g.prg[a]);
    if (a < 0xc0000) return be16(&g.word[a & 0x3ffff]);
    if (a >= 0xff8000) return sub_ga_rd(a);
    return (rd8(a) << 8) | rd8(a + 1);
}

void SubBus::wr8(uint32_t a, uint8_t v) {
    if (a < 0x80000) { g.prg[a] = v; return; }
    if (a < 0xc0000) { g.word[a & 0x3ffff] = v; return; }
    if (a >= 0xfe0000 && a < 0xff0000) { if (a & 1) g.bram[(a >> 1) & 0x1fff] = v; return; }
    if (a >= 0xff0000 && a < 0xff8000) { if (a & 1) pcm_write(a, v); return; }
    if (a >= 0xff8000) { sub_ga_wr8(a, v); return; }
}

void SubBus::wr16(uint32_t a, uint16_t v) {
    if (a < 0x80000) { wbe16(&g.prg[a], v); return; }
    if (a < 0xc0000) { wbe16(&g.word[a & 0x3ffff], v); return; }
    if (a >= 0xff8000) {
        uint32_t r = a & 0x1ff;
        if (r == 0x002) { sub_ga_wr8(a + 1, (uint8_t)v); return; }
        if (r == 0x00e) { g.flag_sub = (uint8_t)v; return; }
        if (r == 0x00a) { g.dma_addr = v; return; }
        if (r == 0x004) { sub_ga_wr8(a, v >> 8); return; }
        if (r >= 0x20 && r < 0x30) { g.stat[(r - 0x20) >> 1] = v; return; }
    }
    wr8(a, v >> 8);
    wr8(a + 1, (uint8_t)v);
}

// ---------------------------------------------------------------- boot (what the BIOS does)

bool Scd::boot(const std::string& path) {
    std::vector<uint8_t> head(0x8000);
    bool cue = path.size() > 4 && (path.compare(path.size() - 4, 4, ".cue") == 0 || path.compare(path.size() - 4, 4, ".CUE") == 0);
    if (cue) {                                  // the player's disc image, read in place
        if (!g_disc.open(path)) return false;
        size_t k = path.find_last_of("/\\");
        game_dir = k == std::string::npos ? std::string(".") : path.substr(0, k);
        for (uint32_t s = 0; s < 16; s++) g_disc.read_data(s, &head[s * 2048]);
        for (auto& t : g_disc.tracks) {
            if (cd.ntracks >= 100) break;
            CdHle::Track& c = cd.tracks[cd.ntracks++];
            c.n = t.n; c.start = (int)t.disc_start; c.len = (int)t.len; c.audio = t.audio;
        }
    } else {                                    // development: a folder made by tools/prepare_disc.py
        game_dir = path;
        std::string p = path + "/track01.iso";
        cd.iso = fopen(p.c_str(), "rb");
        if (!cd.iso) { fprintf(stderr, "cannot open %s\n", p.c_str()); return false; }
        if (fread(head.data(), 1, head.size(), cd.iso) != head.size()) return false;
        cd.load_tracks((path + "/tracks.txt").c_str());
    }
    if (memcmp(head.data(), "SEGADISCSYSTEM", 14)) {
        fprintf(stderr, "%s is not a Sega CD disc\n", path.c_str());
        return false;
    }
    memcpy(&wram[0], &head[0x200], 0x6600);     // IP -> $FF0000
    memcpy(&prg[0x6000], &head[0x6800], 0x1800);// SP -> $6000
    // SP module header: word offsets of init/main/int2/user at +$18 (relative to that table)
    uint32_t tab = 0x6000 + ((prg[0x6018] << 24) | (prg[0x6019] << 16) | (prg[0x601a] << 8) | prg[0x601b]);
    for (int k = 0; k < 4; k++) sp_entry[k] = tab + be16(&prg[tab + 2 * k]);
    fprintf(stderr, "SP entries: init %06x main %06x int2 %06x user %06x\n", sp_entry[0], sp_entry[1], sp_entry[2], sp_entry[3]);
    vdp.reset();
    main.bus = &mbus; main.id = CPU_MAIN;
    sub.bus = &sbus; sub.id = CPU_SUB;
    main.set_sr(0x2700); main.a[7] = 0xfffd00;
    sub.set_sr(0x2700); sub.a[7] = 0x5e80;
    sub_reset = false; sub_busreq = false;      // the BIOS has started the sub CPU
    sub_imask = 0x04;                            // level 2 enabled
    return true;
}
