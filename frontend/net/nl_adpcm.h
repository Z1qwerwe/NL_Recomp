// IMA-ADPCM fuer den Online-Ton: 4 Bit pro Abtastwert statt 16 -> ca. 1/4 der Datenmenge.
// Jedes Paket ist fuer sich dekodierbar (Startwert und Index stehen im Kopf).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nladpcm {

inline const int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct State { int pred = 0; int index = 0; };

inline uint8_t encode_sample(State& s, int sample) {
    const int step = kStep[s.index];
    int diff = sample - s.pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    s.pred = std::clamp(s.pred + ((code & 8) ? -delta : delta), -32768, 32767);
    s.index = std::clamp(s.index + kIndex[code], 0, 88);
    return code;
}

inline int16_t decode_sample(State& s, uint8_t code) {
    const int step = kStep[s.index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    s.pred = std::clamp(s.pred + ((code & 8) ? -delta : delta), -32768, 32767);
    s.index = std::clamp(s.index + kIndex[code & 15], 0, 88);
    return (int16_t)s.pred;
}

// Stereo: Kopf = je Kanal int16 Startwert + uint8 Index (6 Byte), danach je Abtastpaar ein Byte (L unten, R oben).
// encoder_state wird ueber Pakete weitergefuehrt; der Kopf macht jedes Paket trotzdem eigenstaendig.
inline void encode_stereo(State st[2], const int16_t* pcm, int frames, std::vector<uint8_t>& out) {
    out.resize(6 + (size_t)frames);
    for (int c = 0; c < 2; c++) {
        int16_t p = (int16_t)st[c].pred;
        memcpy(out.data() + c * 3, &p, 2);
        out[c * 3 + 2] = (uint8_t)st[c].index;
    }
    for (int i = 0; i < frames; i++) {
        const uint8_t l = encode_sample(st[0], pcm[i * 2]);
        const uint8_t r = encode_sample(st[1], pcm[i * 2 + 1]);
        out[6 + i] = (uint8_t)(l | (r << 4));
    }
}

inline bool decode_stereo(const uint8_t* data, size_t size, std::vector<int16_t>& pcm) {
    if (size < 6) return false;
    State st[2];
    for (int c = 0; c < 2; c++) {
        int16_t p;
        memcpy(&p, data + c * 3, 2);
        st[c].pred = p;
        st[c].index = std::min<int>(data[c * 3 + 2], 88);
    }
    const size_t frames = size - 6;
    pcm.resize(frames * 2);
    for (size_t i = 0; i < frames; i++) {
        const uint8_t b = data[6 + i];
        pcm[i * 2] = decode_sample(st[0], b & 15);
        pcm[i * 2 + 1] = decode_sample(st[1], b >> 4);
    }
    return true;
}

}  // namespace nladpcm
