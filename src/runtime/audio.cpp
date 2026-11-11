// Audio mixing: CD-DA from the extracted tracks (44.1 kHz stereo), plus the PCM chip and FM when
// present. Samples are produced per emulated frame, positioned by the emulated CD clock.
#include <cstring>
#include <map>
#include "audio.h"
#include "system.h"
#include "disc.h"

static const int64_t SUB_HZ = 12500000;

static FILE* track_file(int n) {
    static std::map<int, FILE*> files;
    auto it = files.find(n);
    if (it != files.end()) return it->second;
    char name[32];
    snprintf(name, sizeof name, "/track%02d.raw", n);
    FILE* f = fopen((g.game_dir + name).c_str(), "rb");
    files[n] = f;
    return f;
}

// CD-DA: sample index (in the current track) the drive is at, from the emulated clock
static bool cdda_position(int64_t now, int& track, int64_t& sample) {
    CdHle& cd = g.cd;
    cd.update(now);
    if (!cd.play_mode || cd.paused || !cd.audio_track) return false;
    int64_t since = (now - cd.play_t0) * 44100 / SUB_HZ;
    int64_t from = (int64_t)cd.play_from * 588 + since;              // disc position in samples
    for (int k = 0; k < cd.ntracks; k++)
        if (cd.tracks[k].audio && from >= (int64_t)cd.tracks[k].start * 588 &&
            from < (int64_t)(cd.tracks[k].start + cd.tracks[k].len) * 588) {
            track = cd.tracks[k].n;
            sample = from - (int64_t)cd.tracks[k].start * 588;
            return true;
        }
    return false;
}

void audio_frame(std::vector<int16_t>& out) {
    // samples for one NTSC frame (44100 / 59.92), carried fractionally
    static double carry = 0;
    double want = 44100.0 / 59.922743 + carry;
    int n = (int)want;
    carry = want - n;
    out.assign(n * 2, 0);

    int track;
    int64_t sample;
    if (cdda_position(g.sub.cycles, track, sample)) {
        static int last_track = -1;
        static int64_t cursor = 0;
        // follow the drive: start where it is, keep continuity while it plays the same stretch
        int64_t drift = sample - (cursor + n);
        if (track != last_track || drift > 4410 || drift < -4410) cursor = sample - n;
        last_track = track;
        if (cursor < 0) cursor = 0;
        if (!g_disc.tracks.empty()) {
            g_disc.read_audio(track, (uint64_t)cursor, out.data(), (size_t)n);
            cursor += n;
        } else if (FILE* f = track_file(track)) {
            std::vector<int16_t> buf(n * 2, 0);
            fseek(f, (long)(cursor * 4), SEEK_SET);
            size_t got = fread(buf.data(), 4, n, f);
            for (size_t k = 0; k < got * 2; k++) out[k] = buf[k];      // raw files are little-endian
            cursor += n;
        }
        int vol = g.cd_volume;     // 0..1024
        for (auto& s : out) s = (int16_t)(s * vol / 1024);
    }
    pcm_mix(out);
    fm_mix(out);
}
