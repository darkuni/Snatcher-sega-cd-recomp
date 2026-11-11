// YM2612 (OPN2) via ymfm. Snatcher writes it from the main 68000 ($A04000-$A04003). The chip runs at
// the main CPU clock (7.67 MHz), one native sample per 144 clocks (~53.27 kHz); it is caught up to the
// main CPU's cycle count before every access and resampled to 44.1 kHz for mixing.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "audio.h"
#include "system.h"
#include "../third_party/ymfm/ymfm_opn.h"

namespace {
struct FmIntf : ymfm::ymfm_interface {
    int64_t clk = 0;
    int64_t timer_at[2] = {-1, -1};
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override {
        if (tnum < 2) timer_at[tnum] = duration < 0 ? -1 : clk + duration;
    }
    void advance(int64_t clocks) {
        clk += clocks;
        for (int t = 0; t < 2; t++)
            if (timer_at[t] >= 0 && clk >= timer_at[t]) {
                timer_at[t] = -1;
                m_engine->engine_timer_expired((uint32_t)t);
            }
    }
};

FmIntf s_intf;
ymfm::ym2612 s_chip(s_intf);
bool s_reset = false;
int64_t s_last = 0;             // main CPU cycle the chip has been run up to
std::vector<int16_t> s_out;     // resampled 44.1 kHz stereo, waiting for the mixer
double s_next = 0;              // resampler: time of the next 44.1 kHz sample, in native samples
int64_t s_n = 0;                // native samples generated
int32_t s_prev[2] = {0, 0}, s_cur[2] = {0, 0};
double s_dcx[2] = {0, 0}, s_dcy[2] = {0, 0};     // DC blocker (the YM2612 DAC has an offset)
const double NATIVE = 7670453.0 / 144.0;
}

static void fm_catchup(int64_t now) {
    if (!s_reset) { s_chip.reset(); s_reset = true; s_last = now; }
    while (s_last + 144 <= now) {
        ymfm::ym2612::output_data o;
        s_chip.generate(&o);
        s_intf.advance(144);
        s_last += 144;
        s_prev[0] = s_cur[0]; s_prev[1] = s_cur[1];
        s_cur[0] = o.data[0]; s_cur[1] = o.data[1];
        // linear interpolation between native samples n-1 and n
        s_n++;
        while (s_next <= (double)s_n) {
            double f = s_next - (double)(s_n - 1);
            for (int ch = 0; ch < 2; ch++) {
                double x = (s_prev[ch] + (s_cur[ch] - s_prev[ch]) * f) * 4;     // ymfm OPN2 output is ~14-bit
                double y = x - s_dcx[ch] + 0.9995 * s_dcy[ch];
                s_dcx[ch] = x; s_dcy[ch] = y;
                int v = (int)y;
                s_out.push_back((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
            }
            s_next += NATIVE / 44100.0;
        }
    }
    if (s_out.size() > 44100 * 2) s_out.erase(s_out.begin(), s_out.end() - 4410 * 2);
}

uint8_t fm_read(uint32_t a) {
    fm_catchup(g.main.cycles);
    (void)a;
    return s_chip.read_status() & 0x7f;     // never report busy (writes land instantly here)
}

void fm_write(uint32_t a, uint8_t v) {
    fm_catchup(g.main.cycles);
    static uint8_t last_addr[2];
    static int logged = 0;
    if (!(a & 1)) last_addr[(a >> 1) & 1] = v;
    else if (getenv("SNATCHER_FMLOG") && logged < 400) {
        logged++;
        fprintf(stderr, "fm f%llu p%d %02x=%02x\n", (unsigned long long)g.frame, (int)((a >> 1) & 1), last_addr[(a >> 1) & 1], v);
    }
    s_chip.write(a & 3, v);
}

void fm_mix(std::vector<int16_t>& out) {
    fm_catchup(g.main.cycles);
    size_t n = out.size() < s_out.size() ? out.size() : s_out.size();
    for (size_t k = 0; k < n; k++) {
        int v = out[k] + s_out[k] * g.fm_volume / 1024;
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    s_out.erase(s_out.begin(), s_out.begin() + n);
    if (s_out.size() > 4410 * 2) s_out.erase(s_out.begin(), s_out.end() - 4410 * 2);
}
