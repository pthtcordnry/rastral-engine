#ifndef MUSIC_DIRECTOR_H
#define MUSIC_DIRECTOR_H
#include <unordered_map>
#include <string>
#include <cstdint>
#include <cstdlib>
#include "MiniAudioEngine.cpp"

enum MD_State { MD_Calm, MD_Tense, MD_Combat, MD_Overdrive };

const char* StateName(MD_State s) {
    switch (s) {
    case MD_Calm:      return "Calm";
    case MD_Tense:     return "Tense";
    case MD_Combat:    return "Combat";
    case MD_Overdrive: return "Overdrive";
    default:           return "?";
    }
}

struct MD_StemDesc {
    std::string  name;
    std::string  filepath;
    bool startActive = false;
    audio_route    route = MAE_ThroughLPF;
};

struct MD_Settings {
    float bpm = 120.f;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    float initialStartDelaySec = 0.10f;
};

struct MD_Stem {
    float currentVol;
    float startVol;
    float targetVol;
    double fadeTime;
    double fadeDur;
    std::string name;
    MD_Stem() : currentVol(0.f), startVol(0.f), targetVol(0.f), fadeTime(0.0), fadeDur(0.0) {}
};

struct MusicDirector {
    audio_engine* eng;
    MD_Settings cfg;
    std::unordered_map<std::string, MD_Stem> stems;

    bool      running;
    MD_State  state;
    float     rage;

    float     targetPitch;
    float     currentPitch;

    MusicDirector()
        : eng(NULL), running(false), state(MD_Calm), rage(0.f),
        targetPitch(1.f), currentPitch(1.f) {
    }
};

#endif // !MUSIC_DIRECTOR_H