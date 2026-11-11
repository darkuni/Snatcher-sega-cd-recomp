// Headless runner (until the SDL front end exists): boots the game and runs N frames.
//   snatcher <game dir> [frames]
#include <cstdio>
#include <cstdlib>
#include "system.h"

void run_machine();
static uint64_t s_frames = 600;

void frame_done() {
    if (g.frame % 60 == 0)
        fprintf(stderr, "frame %llu  vint %u ifl2 %u int2 %u sectors %u  vdp r1 %02x  vint@%06x  flags %02x/%02x  sub state $7840=%04x\n",
                (unsigned long long)g.frame, g.n_vint, g.n_ifl2, g.n_int2, g.n_sectors, g.vdp.reg[1], g.main_vint,
                g.flag_main, g.flag_sub, be16(&g.prg[0x7840]));
    if (g.force_interp && g.frame % 60 == 0) fprintf(stderr, "   pc main %06x sub %06x\n", g.main.dbg_pc, g.sub.dbg_pc);
    if (g.frame >= s_frames) {
        FILE* f = fopen("misses.txt", "w");
        for (int c = 0; c < 2; c++)
            for (uint32_t a : g.misses[c]) if (f) fprintf(f, "%s %06x\n", c ? "sub" : "main", a);
        if (f) fclose(f);
        fprintf(stderr, "done: %llu frames, %zu main / %zu sub addresses interpreted (misses.txt)\n",
                (unsigned long long)g.frame, g.misses[0].size(), g.misses[1].size());
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
