#ifndef MINIAUDIO_ENGINE_H
#define MINIAUDIO_ENGINE_H

#define MA_ENABLE_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "../Include/miniaudio.h"

#include <unordered_map>

enum audio_route {
    MAE_ThroughLPF = 0,
    MAE_BypassLPF = 1
};

struct audio_config {
    int    sampleRate = 0;
    int    channels = 0;
    double lpfStartHz = 18000.0;
    int    lpfOrder = 4;
};

struct audio_sound {
    ma_sound    sound{};
    bool        loaded = false;
    audio_route route = MAE_ThroughLPF;
    float       baseVol = 1.f;
};

struct audio_engine {
    audio_config  cfg{};
    ma_engine     engine{};
    bool          engineInit = false;

    ma_hpf_node   hpf{};
    bool          hpfInit = false;

    ma_lpf_node   lpf{};
    bool          lpfInit = false;

    ma_uint32     sampleRate = 48000;
    ma_uint32     channels = 2;
    float         routeGain[2]{ 1.f, 1.f };

    std::unordered_map<std::string, audio_sound> sounds;
};

#endif