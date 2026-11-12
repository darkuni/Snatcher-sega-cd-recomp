// Mega Drive VDP: command/data ports, DMA (68k -> VDP, fill, copy) and a scanline renderer
// (planes A/B with full/cell/line H-scroll and full/2-cell V-scroll, window plane, sprites,
// layer priority). Shadow/highlight is not implemented yet.
#include "vdp.h"
#include <cstdlib>
#include <cstring>
#include "system.h"

static const uint8_t LEVEL[8] = {0, 52, 87, 116, 144, 172, 206, 255};

void Vdp::reset() {
    memset(vram, 0, sizeof vram);
    memset(cram, 0, sizeof cram);
    memset(vsram, 0, sizeof vsram);
    memset(reg, 0, sizeof reg);
    memset(fb, 0, sizeof fb);
    reg[15] = 2;
}

uint16_t Vdp::status(int line) const {
    return 0x3600 | (vint_flag ? 0x80 : 0) | (line >= height() ? 8 : 0);
}

void Vdp::ctrl_write(uint16_t v) {
    {
        static const char* cl = getenv("SNATCHER_CTRLLOG");
        static int n = 0;
        if (cl && g.frame == (uint64_t)atoi(cl) && n++ < 60)
            fprintf(stderr, "ctrl %04x pending=%d line %d\n", v, pending, g.line);
    }
    if (!pending) {
        if ((v & 0xc000) == 0x8000) {               // register write
            int r = (v >> 8) & 0x1f;
            if (r < 24) reg[r] = (uint8_t)v;
            code &= 0x3c;                           // writing a register clears CD1-0 (as hardware)
            return;
        }
        cmd1 = v;
        pending = true;
        code = (code & 0x3c) | (v >> 14);
        addr = (addr & 0xc000) | (v & 0x3fff);
        return;
    }
    pending = false;
    code = (code & 0x03) | ((v >> 2) & 0x3c);
    addr = (addr & 0x3fff) | ((v & 3) << 14);
    if ((code & 0x20) && (reg[1] & 0x10)) {        // DMA requested and enabled
        if ((reg[23] & 0xc0) == 0x80) fill_pending = true;
        else do_dma();
    }
}

void Vdp::write_target(uint16_t v) {
    switch (code & 0x0f) {
    case 1:                                          // VRAM
        if (addr & 1) v = (v >> 8) | (v << 8);
        vram[addr & 0xfffe] = v >> 8;
        vram[(addr & 0xfffe) | 1] = (uint8_t)v;
        break;
    case 3: cram[(addr >> 1) & 63] = v & 0x0eee; break;
    case 5: if (((addr >> 1) & 63) < 40) vsram[(addr >> 1) & 63] = v & 0x07ff; break;
    }
}

void Vdp::data_write(uint16_t v) {
    pending = false;
    {
        static const char* wl = getenv("SNATCHER_VWLOG");
        if (wl && (code & 15) == 1 && addr < 0xf800 && g.frame >= (uint64_t)atoi(wl) && g.frame < (uint64_t)atoi(wl) + 120) {
            static uint64_t lf = 0; static uint32_t first = 0, last = 0, n = 0;
            if (g.frame != lf || addr != ((last + reg[15]) & 0xffff)) {
                if (n) fprintf(stderr, "vram f%llu %04x..%04x (%u words)\n", (unsigned long long)lf, first, last, n);
                first = addr; n = 0; lf = g.frame;
            }
            last = addr; n++;
        }
    }
    if (fill_pending) {                              // DMA fill: write the word, then fill with its high byte
        fill_pending = false;
        write_target(v);
        uint32_t len = reg[19] | (reg[20] << 8);
        if (!len) len = 0x10000;
        for (uint32_t k = 0; k < len; k++) {
            if ((code & 0x0f) == 1) vram[(addr ^ 1) & 0xffff] = v >> 8;
            else write_target(v);
            advance();
        }
        reg[19] = reg[20] = 0;
        return;
    }
    write_target(v);
    advance();
}

uint16_t Vdp::data_read() {
    pending = false;
    uint16_t v = 0;
    switch (code & 0x0f) {
    case 0: v = (vram[addr & 0xfffe] << 8) | vram[(addr & 0xfffe) | 1]; break;
    case 4: v = vsram[((addr >> 1) & 63) % 40]; break;
    case 8: v = cram[(addr >> 1) & 63]; break;
    }
    advance();
    return v;
}

void Vdp::do_dma() {
    uint32_t len = reg[19] | (reg[20] << 8);
    if (!len) len = 0x10000;
    if ((reg[23] & 0xc0) == 0xc0) {                 // VRAM copy (bytes)
        uint32_t src = reg[21] | (reg[22] << 8);
        for (uint32_t k = 0; k < len; k++) {
            vram[addr & 0xffff] = vram[src & 0xffff];
            src++;
            advance();
        }
    } else {                                         // 68k memory -> VDP (words)
        uint32_t src = ((reg[21] | (reg[22] << 8) | ((reg[23] & 0x7f) << 16)) << 1) & 0xffffff;
        if (getenv("SNATCHER_DMAFILL") && src >= 0x200000 && src < 0x240000 && (code & 15) == 1 && addr < 0x8000) {
            int nz = 0; for (uint32_t k = 0; k < len * 2; k++) nz += g.word[(src - 2 + k) & 0x3ffff] != 0;
            fprintf(stderr, "dma f%llu line %d word %06x -> vram %04x len %u: %d nonzero bytes (ret=%d dmna=%d sub loop %06x)\n",
                    (unsigned long long)g.frame, g.line, src, addr, len, nz, g.ret, g.dmna, g.sub.dbg_pc);
        }
        static const char* dl = getenv("SNATCHER_DMALOG");
        if (dl && g.frame >= (uint64_t)atoi(dl) && g.frame < (uint64_t)atoi(dl) + 40)
            fprintf(stderr, "dma f%llu line %d src %06x -> %s %04x len %u (ret=%d dmna=%d)\n", (unsigned long long)g.frame, g.line, src,
                    (code & 15) == 1 ? "vram" : (code & 15) == 3 ? "cram" : "vsram", addr, len, g.ret, g.dmna);
        // Sega CD: DMA from word RAM delivers each word one slot late - games add 2 to the source
        bool wordram = src >= 0x200000 && src < 0x240000;
        for (uint32_t k = 0; k < len; k++) {
            uint32_t a = wordram ? src - 2 : src;
            write_target((uint16_t)g.mbus.rd16(a));
            src = (src & 0xfe0000) | ((src + 2) & 0x1ffff);    // source wraps in 128 KB
            advance();
        }
    }
    reg[19] = reg[20] = 0;
    code &= ~0x20;
}

// ---------------------------------------------------------------- renderer

static inline uint32_t rgb(uint16_t c) {
    return (LEVEL[(c >> 1) & 7] << 16) | (LEVEL[(c >> 5) & 7] << 8) | LEVEL[(c >> 9) & 7];
}

// one pixel of a layer: bits 0-5 colour index (palette*16 + index), bit 6 priority, 0 index = transparent
static inline uint8_t tile_px(const uint8_t* vram, uint16_t entry, int px, int py) {
    if (entry & 0x0800) px = 7 - px;
    if (entry & 0x1000) py = 7 - py;
    uint32_t t = ((entry & 0x7ff) << 5) + py * 4 + (px >> 1);
    uint8_t b = vram[t & 0xffff];
    uint8_t ci = (px & 1) ? (b & 15) : (b >> 4);
    if (!ci) return 0;
    return ci | (((entry >> 13) & 3) << 4) | ((entry & 0x8000) ? 0x40 : 0);
}

void Vdp::render_line(int line) {
    if (line >= H) return;
    uint32_t* out = &fb[line * W];
    int w = width();
    uint32_t bg = rgb(cram[reg[7] & 63]);
    if (!(reg[1] & 0x40)) {                          // display disabled
        for (int x = 0; x < W; x++) out[x] = x < w ? bg : 0;
        return;
    }
    static const int PSZ[4] = {32, 64, 32, 128};
    int pw = PSZ[reg[16] & 3], ph = PSZ[(reg[16] >> 4) & 3];
    uint32_t nta = (reg[2] & 0x38) << 10, ntb = (reg[4] & 7) << 13;
    uint32_t hsa = (reg[13] & 0x3f) << 10;
    int hmode = reg[11] & 3;
    bool vcell = reg[11] & 4;

    uint8_t la[320], lb[320], ls[320];
    // planes
    for (int p = 0; p < 2; p++) {
        uint8_t* dst = p ? lb : la;
        uint32_t nt = p ? ntb : nta;
        int hrow = hmode == 0 ? 0 : hmode == 2 ? (line & ~7) : line;
        int hs = ((vram[(hsa + hrow * 4 + p * 2) & 0xffff] << 8) | vram[(hsa + hrow * 4 + p * 2 + 1) & 0xffff]) & 0x3ff;
        for (int x = 0; x < w; x++) {
            int vs = vsram[vcell ? ((x / 16) * 2 + p) % 40 : p] & 0x3ff;
            int sx = (x - hs) & (pw * 8 - 1);
            int sy = (line + vs) & (ph * 8 - 1);
            uint32_t ea = nt + ((sy >> 3) * pw + (sx >> 3)) * 2;
            uint16_t entry = (vram[ea & 0xffff] << 8) | vram[(ea + 1) & 0xffff];
            dst[x] = tile_px(vram, entry, sx & 7, sy & 7);
        }
    }
    // window (replaces plane A where it is active)
    {
        int wh = (reg[17] & 0x1f) * 16, wv = (reg[18] & 0x1f) * 8;
        bool wright = reg[17] & 0x80, wdown = reg[18] & 0x80;
        bool rowwin = wdown ? line >= wv : line < wv;
        uint32_t wnt = (reg[3] & (w == 320 ? 0x3c : 0x3e)) << 10;
        int wpw = w == 320 ? 64 : 32;
        for (int x = 0; x < w; x++) {
            bool colwin = wright ? x >= wh : x < wh;
            if (!(rowwin || (wh && colwin))) continue;
            if (!wh && !wv) continue;
            uint32_t ea = wnt + ((line >> 3) * wpw + (x >> 3)) * 2;
            uint16_t entry = (vram[ea & 0xffff] << 8) | vram[(ea + 1) & 0xffff];
            la[x] = tile_px(vram, entry, x & 7, line & 7);
        }
    }
    // sprites
    memset(ls, 0, sizeof ls);
    {
        uint32_t sat = (reg[5] & (w == 320 ? 0x7e : 0x7f)) << 9;
        int maxs = w == 320 ? 80 : 64, maxl = w == 320 ? 20 : 16, n = 0, idx = 0;
        for (int k = 0; k < maxs; k++) {
            const uint8_t* s = &vram[(sat + idx * 8) & 0xffff];
            int y = (((s[0] << 8) | s[1]) & 0x3ff) - 128;
            int hs = ((s[2] >> 0) & 3) + 1, ws = ((s[2] >> 2) & 3) + 1;
            int link = s[3] & 0x7f;
            uint16_t attr = (s[4] << 8) | s[5];
            int x = (((s[6] << 8) | s[7]) & 0x1ff) - 128;
            if (line >= y && line < y + hs * 8) {
                if (++n > maxl) break;
                int py = line - y;
                if (attr & 0x1000) py = hs * 8 - 1 - py;
                for (int cx = 0; cx < ws * 8; cx++) {
                    int sx = x + cx;
                    if (sx < 0 || sx >= w || (ls[sx] & 15)) continue;
                    int px = (attr & 0x0800) ? ws * 8 - 1 - cx : cx;
                    uint16_t tile = (attr & 0x7ff) + (px >> 3) * hs + (py >> 3);
                    uint8_t v = tile_px(vram, (attr & 0xe000) | (tile & 0x7ff), px & 7, py & 7);
                    if (v) ls[sx] = v;
                }
            }
            if (!link) break;
            idx = link;
        }
    }
    // priority: B lo < A lo < S lo < B hi < A hi < S hi
    for (int x = 0; x < W; x++) {
        if (x >= w) { out[x] = 0; continue; }
        uint8_t c = 0;
        int best = -1;
        auto take = [&](uint8_t v, int rank) { if ((v & 15) && rank > best) { best = rank; c = v; } };
        take(lb[x], (lb[x] & 0x40) ? 3 : 0);
        take(la[x], (la[x] & 0x40) ? 4 : 1);
        take(ls[x], (ls[x] & 0x40) ? 5 : 2);
        out[x] = best < 0 ? bg : rgb(cram[c & 63]);
    }
}
