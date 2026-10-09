// ndskit runtime — the sound to the host: the audio device, WAV files.
//
// Each frame the sound unit's samples (stereo, 16 bits, 32 728.5 Hz: written
// as 32 728 in a WAV's header) go to an SDL3 audio stream, which resamples
// them to the device's rate, and to the WAV file if one is asked for. The
// window keeps the DS's pace by the host's clock (display.cpp) while the
// device plays by its own: the stream's queue is held near three frames by
// nudging its playback ratio, by at most 0.5% (a twelfth of a semitone).
// A queue run dry (the window fell behind) plays silence until the next
// frame; one grown past half a second is cut back.
#include "display.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

SDL_AudioStream* stream;
FILE* wav;
uint64_t wav_bytes;
double queued_avg;
uint64_t frames, dry, cuts;                         // what the window's run went through
double ratio_sum, ratio_lo = 2, ratio_hi;
const int kRate = 32728;
const double kTarget = 3 * 548.0 * 4;               // three frames, in bytes of the stream's input

void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

void wav_header() {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    put32(h + 4, (uint32_t)(36 + wav_bytes));
    put32(h + 24, kRate);
    put32(h + 28, kRate * 4);
    put16(h + 32, 4);
    put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)wav_bytes);
    std::fseek(wav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, wav);
    std::fseek(wav, 0, SEEK_END);
}

}  // namespace

bool audio_open(bool device, const std::string& wav_path) {
    if (!wav_path.empty()) {
        wav = std::fopen(wav_path.c_str(), "wb");
        if (!wav) { nds_note("audio: cannot write %s", wav_path.c_str()); return false; }
        wav_header();
    }
    if (device) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { nds_note("audio: SDL: %s", SDL_GetError()); return false; }
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) { nds_note("audio: no device: %s", SDL_GetError()); return false; }
        std::vector<int16_t> silence((size_t)kTarget / 2);
        SDL_PutAudioStreamData(stream, silence.data(), (int)kTarget);
        queued_avg = kTarget;
        SDL_ResumeAudioStreamDevice(stream);
        SDL_AudioSpec dev;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, nullptr))
            nds_note("audio: %s, %d Hz", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream)), dev.freq);
    }
    g_sound_keep = device || wav;
    return true;
}

void audio_frame() {
    std::vector<int16_t>& s = sound_samples();
    if (wav && !s.empty()) {
        std::fwrite(s.data(), 2, s.size(), wav);
        wav_bytes += 2 * s.size();
        wav_header();
    }
    if (stream) {
        int q = SDL_GetAudioStreamQueued(stream);
        if (q > kRate * 2) {                        // half a second
            SDL_ClearAudioStream(stream);
            q = 0;
            ++cuts;
        } else if (q == 0) {
            ++dry;
        }
        SDL_PutAudioStreamData(stream, s.data(), (int)(2 * s.size()));
        queued_avg += (q - queued_avg) * 0.05;
        double r = 1 + std::clamp((queued_avg - kTarget) / kTarget * 0.01, -0.005, 0.005);
        SDL_SetAudioStreamFrequencyRatio(stream, (float)r);
        ++frames;
        ratio_sum += r;
        ratio_lo = std::min(ratio_lo, r);
        ratio_hi = std::max(ratio_hi, r);
    }
    s.clear();
}

void audio_close() {
    if (wav) { wav_header(); std::fclose(wav); wav = nullptr; }
    if (stream) {
        if (frames)
            nds_note("audio: %llu frames, the queue ran dry %llu times and was cut %llu; playback ratio %.4f (%.4f-%.4f)",
                     (unsigned long long)frames, (unsigned long long)dry, (unsigned long long)cuts, ratio_sum / frames,
                     ratio_lo, ratio_hi);
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
}
