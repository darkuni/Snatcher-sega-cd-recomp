// Ricoh RF5C164 PCM (sub CPU $FF0000-$FF3FFF, odd bytes): 8 channels over 64 KB wave RAM.
// Runs on the emulated clock (pcm_catchup before every register access) because games stream
// samples by watching the channels' address counters. Output is produced at 44.1 kHz.
#include <cstring>
#include "audio.h"
#include "system.h"

static const int64_t SUB_HZ = 12500000;
static const double CHIP_HZ = 12500000.0 / 384.0;   // 32552 Hz

struct PcmCh {
    uint8_t env = 0, pan = 0xff;
    uint16_t fd = 0, ls = 0;
    uint8_t st = 0;
    uint32_t addr = 0;          // 16.11 fixed point
};

static PcmCh s_ch[8];
static uint8_t s_ctrl = 0, s_onoff = 0xff, s_sel = 0, s_bank = 0;
static int64_t s_last = 0;
static double s_frac = 0;
static std::vector<int16_t> s_out;      // pending 44.1 kHz stereo samples

static void step_one(int& l, int& r) {
    l = r = 0;
    if (!(s_ctrl & 0x80)) return;                       // sounding disabled
    const double scale = CHIP_HZ / 44100.0;
    for (int c = 0; c < 8; c++) {
        PcmCh& ch = s_ch[c];
        if (s_onoff & (1 << c)) continue;               // active low
        uint8_t s = g.pcm[(ch.addr >> 11) & 0xffff];
        if (s == 0xff) {                                // loop marker
            ch.addr = (uint32_t)ch.ls << 11;
            s = g.pcm[ch.ls];
            if (s == 0xff) continue;
        }
        int v = (s & 0x80) ? (s & 0x7f) : -(s & 0x7f);  // sign-magnitude, bit 7 = positive
        v *= ch.env;
        l += v * (ch.pan & 15) >> 5;
        r += v * (ch.pan >> 4) >> 5;
        ch.addr = (ch.addr + (uint32_t)(ch.fd * scale)) & 0x7ffffff;
    }
}

void pcm_catchup(int64_t now) {
    if (now <= s_last) return;
    double want = (double)(now - s_last) * 44100.0 / SUB_HZ + s_frac;
    int n = (int)want;
    s_frac = want - n;
    s_last = now;
    if (s_out.size() > 44100 * 2) s_out.clear();         // nobody drains (headless): drop
    for (int k = 0; k < n; k++) {
        int l, r;
        step_one(l, r);
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        s_out.push_back((int16_t)l);
        s_out.push_back((int16_t)r);
    }
}

uint8_t pcm_read(uint32_t a) {
    pcm_catchup(g.sub.cycles);
    uint32_t off = a & 0x7fff;
    if (off >= 0x2000 && off < 0x4000) return g.pcm[s_bank * 0x1000 + ((off - 0x2000) >> 1)];
    if (off >= 0x20 && off < 0x40) {                    // address counters (play position, integer part)
        int idx = (off - 0x20) >> 1, c = idx >> 1;
        uint32_t p = s_ch[c].addr >> 11;
        return (idx & 1) ? (uint8_t)(p >> 8) : (uint8_t)p;
    }
    return 0;
}

uint32_t g_pcm_writes = 0, g_pcm_on = 0;
void pcm_write(uint32_t a, uint8_t v) {
    pcm_catchup(g.sub.cycles);
    g_pcm_writes++;
    uint32_t off = a & 0x7fff;
    if (off >= 0x2000 && off < 0x4000) { g.pcm[s_bank * 0x1000 + ((off - 0x2000) >> 1)] = v; return; }
    if (off >= 0x20) return;
    PcmCh& ch = s_ch[s_sel];
    switch (off >> 1) {
    case 0: ch.env = v; break;
    case 1: ch.pan = v; break;
    case 2: ch.fd = (ch.fd & 0xff00) | v; break;
    case 3: ch.fd = (ch.fd & 0x00ff) | (v << 8); break;
    case 4: ch.ls = (ch.ls & 0xff00) | v; break;
    case 5: ch.ls = (ch.ls & 0x00ff) | (v << 8); break;
    case 6: ch.st = v; break;
    case 7:
        s_ctrl = v;
        if (v & 0x40) s_sel = v & 7; else s_bank = v & 15;
        break;
    case 8: {
        uint8_t turned_on = s_onoff & ~v;                // bits going 1 -> 0 start a channel
        for (int c = 0; c < 8; c++)
            if (turned_on & (1 << c)) { s_ch[c].addr = (uint32_t)s_ch[c].st << 19; g_pcm_on++; }
        s_onoff = v;
        break;
    }
    }
}

void pcm_dma_write(uint32_t off, uint8_t v) { g.pcm[(s_bank * 0x1000 + off) & 0xffff] = v; }

void pcm_mix(std::vector<int16_t>& out) {
    pcm_catchup(g.sub.cycles);
    size_t n = out.size() < s_out.size() ? out.size() : s_out.size();
    for (size_t k = 0; k < n; k++) {
        int v = out[k] + s_out[k] * g.pcm_volume / 1024;
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    s_out.erase(s_out.begin(), s_out.begin() + n);
    if (s_out.size() > 4410 * 2) s_out.erase(s_out.begin(), s_out.end() - 4410);   // keep latency bounded
}
