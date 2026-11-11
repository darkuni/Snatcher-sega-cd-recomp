// .cue/.bin reader. Handles one .bin for the whole disc (redump style with a single FILE) and one
// .bin per track (multiple FILE lines). Sectors are 2352 bytes (MODE1/2352 data: user data at +16).
#include "disc.h"
#include <cstring>

Disc g_disc;

static std::string dir_of(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k + 1);
}

static uint64_t file_size(FILE* f) {
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    return s < 0 ? 0 : (uint64_t)s;
}

bool Disc::open(const std::string& cue) {
    FILE* c = fopen(cue.c_str(), "r");
    if (!c) { fprintf(stderr, "cannot open %s\n", cue.c_str()); return false; }
    char line[1024];
    std::vector<uint32_t> idx0;          // INDEX 00 per track (or ~0)
    while (fgets(line, sizeof line, c)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) {
            char* q1 = strchr(p, '"');
            char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
            if (!q1 || !q2) continue;
            std::string name(q1 + 1, q2);
            FILE* f = fopen((dir_of(cue) + name).c_str(), "rb");
            if (!f) { fprintf(stderr, "cannot open %s\n", (dir_of(cue) + name).c_str()); fclose(c); return false; }
            files.push_back(f);
        } else if (!strncmp(p, "TRACK", 5)) {
            Track t;
            char mode[32] = {};
            sscanf(p, "TRACK %d %31s", &t.n, mode);
            t.audio = !strcmp(mode, "AUDIO");
            t.file = (int)files.size() - 1;
            tracks.push_back(t);
            idx0.push_back(~0u);
        } else if (!strncmp(p, "INDEX", 5) && !tracks.empty()) {
            int i, m, s, fr;
            if (sscanf(p, "INDEX %d %d:%d:%d", &i, &m, &s, &fr) == 4) {
                uint32_t sec = (uint32_t)((m * 60 + s) * 75 + fr);
                if (i == 1) tracks.back().start = sec;
                if (i == 0) idx0.back() = sec;
            }
        }
    }
    fclose(c);
    if (tracks.empty() || files.empty()) { fprintf(stderr, "%s: no tracks\n", cue.c_str()); return false; }
    // lengths: up to the next track in the same file (its INDEX 00 if any), else to the end of the file
    uint32_t disc_pos = 0;
    for (size_t k = 0; k < tracks.size(); k++) {
        Track& t = tracks[k];
        uint32_t end;
        if (k + 1 < tracks.size() && tracks[k + 1].file == t.file)
            end = idx0[k + 1] != ~0u ? idx0[k + 1] : tracks[k + 1].start;
        else
            end = (uint32_t)(file_size(files[t.file]) / 2352);
        t.len = end > t.start ? end - t.start : 0;
        if (k == 0 || tracks[k].file != tracks[k - 1].file) {
            // a new file: the disc timeline continues after the previous track (2 s pregap if none given)
            if (k) disc_pos += (idx0[k] != ~0u ? t.start - idx0[k] : 150);
            t.disc_start = disc_pos + (k ? 0 : t.start);
        } else {
            t.disc_start = tracks[k - 1].disc_start + (t.start - tracks[k - 1].start);
        }
        disc_pos = t.disc_start + t.len;
    }
    return true;
}

const Disc::Track* Disc::track(int n) const {
    for (auto& t : tracks) if (t.n == n) return &t;
    return nullptr;
}

bool Disc::read_data(uint32_t lba, uint8_t out[2048]) {
    const Track& t = tracks[0];
    if (lba >= t.len) { memset(out, 0, 2048); return false; }
    FILE* f = files[t.file];
    fseek(f, (long)((uint64_t)(t.start + lba) * 2352 + 16), SEEK_SET);
    if (fread(out, 1, 2048, f) != 2048) { memset(out, 0, 2048); return false; }
    return true;
}

size_t Disc::read_audio(int n, uint64_t sample, int16_t* out, size_t frames) {
    const Track* t = track(n);
    if (!t || !t->audio) return 0;
    uint64_t total = (uint64_t)t->len * 588;
    if (sample >= total) return 0;
    if (frames > total - sample) frames = (size_t)(total - sample);
    FILE* f = files[t->file];
    fseek(f, (long)((uint64_t)t->start * 2352 + sample * 4), SEEK_SET);
    size_t got = fread(out, 4, frames, f);
    for (size_t k = 0; k < got * 2; k++) {          // CD audio is little-endian 16-bit
        uint8_t* b = (uint8_t*)&out[k];
        out[k] = (int16_t)(b[0] | (b[1] << 8));
    }
    return got;
}
