#pragma once
#include <cstdint>
#include <vector>

void audio_frame(std::vector<int16_t>& out);     // one frame of 44.1 kHz stereo
void pcm_mix(std::vector<int16_t>& out);         // pcm.cpp: add the RF5C164 output
