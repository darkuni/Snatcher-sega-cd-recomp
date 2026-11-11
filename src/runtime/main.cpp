// Headless runner (until the SDL front end exists): boots the game and runs N frames.
//   snatcher <game dir> [frames]
#include <cstdio>
#include <cstdlib>
#include "system.h"

void run_machine();
void print_profile();
static uint64_t s_frames = 600;

static void dump_frame() {
    static int every = getenv("SNATCHER_DUMP") ? atoi(getenv("SNATCHER_DUMP")) : 0;
    if (!every || g.frame % every) return;
    char name[64];
    snprintf(name, sizeof name, "frame_%05llu.ppm", (unsigned long long)g.frame);
    FILE* f = fopen(name, "wb");
    if (!f) return;
    int w = g.vdp.width(), h = g.vdp.height();
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = g.vdp.fb[y * Vdp::W + x];
            uint8_t px[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

void frame_done() {
    dump_frame();
    if (g.frame % 60 == 0)
        fprintf(stderr, "frame %llu  vint %u ifl2 %u int2 %u sectors %u  vdp r1 %02x  vint@%06x  flags %02x/%02x  sub state $7840=%04x\n",
                (unsigned long long)g.frame, g.n_vint, g.n_ifl2, g.n_int2, g.n_sectors, g.vdp.reg[1], g.main_vint,
                g.flag_main, g.flag_sub, be16(&g.prg[0x7840]));
    if (g.frame % 60 == 0) fprintf(stderr, "   state $e022=%04x e04a=%04x e04e=%04x cmd %04x %04x stat %04x %04x  dmna %u ret %u (now ret=%d dmna=%d) $e028=%02x  loop main %06x sub %06x  sub sr %04x imask-reg %02x timer %02x\n", be16(&g.wram[0xe022]), be16(&g.wram[0xe04a]), be16(&g.wram[0xe04e]), g.cmd[0], g.cmd[1], g.stat[0], g.stat[1], g.n_dmna, g.n_ret, g.ret, g.dmna, g.wram[0xe028], g.main.dbg_pc, g.sub.dbg_pc, g.sub.get_sr(), g.sub_imask, g.timer);
    if (g.force_interp && g.frame % 60 == 0) fprintf(stderr, "   pc main %06x sub %06x\n", g.main.dbg_pc, g.sub.dbg_pc);
    if (g.frame >= s_frames) {
        FILE* f = fopen("misses.txt", "w");
        for (int c = 0; c < 2; c++)
            for (uint32_t a : g.misses[c]) if (f) fprintf(f, "%s %06x\n", c ? "sub" : "main", a);
        if (f) fclose(f);
        fprintf(stderr, "done: %llu frames, %zu main / %zu sub addresses interpreted (misses.txt)\n",
                (unsigned long long)g.frame, g.misses[0].size(), g.misses[1].size());
        print_profile();
        { int nz = 0; for (int k = 0; k < 0x10000; k++) nz += g.vdp.vram[k] != 0;
          int cz = 0; for (int k = 0; k < 64; k++) cz += g.vdp.cram[k] != 0;
          fprintf(stderr, "vram nonzero %d bytes, cram nonzero %d entries, regs:", nz, cz);
          for (int r = 0; r < 24; r++) fprintf(stderr, " %02x", g.vdp.reg[r]);
          fprintf(stderr, "\n");
          FILE* f = fopen("vdp.bin", "wb");
          if (f) { fwrite(g.vdp.vram, 1, 0x10000, f); fwrite(g.vdp.cram, 2, 64, f); fwrite(g.vdp.vsram, 2, 40, f); fwrite(g.vdp.reg, 1, 32, f); fclose(f); } }
        fflush(stderr);
        _Exit(0);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <game dir> [frames]\n", argv[0]); return 1; }
    if (argc > 2) s_frames = strtoull(argv[2], nullptr, 10);
    if (!g.boot(argv[1])) return 1;
    g.force_interp = getenv("SNATCHER_INTERP") != nullptr;
    run_machine();
    return 0;
}
