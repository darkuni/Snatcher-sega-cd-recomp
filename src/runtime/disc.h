// The player's disc image: a .cue sheet with its .bin file(s), read directly (no extraction step).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Disc {
    struct Track {
        int n = 0;
        bool audio = false;
        int file = 0;           // index into files
        uint32_t start = 0;     // first sector (INDEX 01) within its file
        uint32_t len = 0;       // sectors up to the next track
        uint32_t disc_start = 0;// first sector on the disc (absolute, from the cue's timeline)
    };
    std::vector<FILE*> files;
    std::vector<Track> tracks;

    bool open(const std::string& cue);
    bool read_data(uint32_t lba, uint8_t out[2048]);          // data track, lba 0 = start of track 1
    size_t read_audio(int track, uint64_t sample, int16_t* out, size_t frames);   // 44.1 kHz stereo
    const Track* track(int n) const;
};

extern Disc g_disc;
