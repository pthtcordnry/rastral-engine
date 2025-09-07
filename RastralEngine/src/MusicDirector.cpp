#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
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

float md_clamp01(float x) {
    return (x < 0.f) ? 0.f : ((x > 1.f) ? 1.f : x);
}

MD_Stem* md_get_stem(MusicDirector* director, const std::string& name) {
    std::unordered_map<std::string, MD_Stem>::iterator it = director->stems.find(name);
    return (it == director->stems.end()) ? (MD_Stem*)0 : &it->second;
}

float md_target_or_zero(const MusicDirector* director, const std::string& name) {
    std::unordered_map<std::string, MD_Stem>::const_iterator it = director->stems.find(name);
    return (it == director->stems.end()) ? 0.f : it->second.targetVol;
}

uint64_t md_seconds_to_frames(const MusicDirector* director, double s) {
    return (uint64_t)std::llround(s * (double)ae_sample_rate(director->eng));
}

double md_frames_to_seconds(const MusicDirector* director, uint64_t f) {
    return (double)f / (double)ae_sample_rate(director->eng);
}

uint64_t md_frames_per_beat(const MusicDirector* director) {
    const double spb = 60.0 / (double)director->cfg.bpm;
    return (uint64_t)std::llround(spb * (double)ae_sample_rate(director->eng));
}

uint64_t md_frames_per_bar(const MusicDirector* director) {
    return md_frames_per_beat(director) * (uint64_t)director->cfg.timeSigNumerator;
}

uint64_t md_next_beat_boundary(const MusicDirector* director, std::uint64_t from) {
    const uint64_t fpb = md_frames_per_beat(director); if (fpb == 0) return from;
    return ((from + fpb - 1) / fpb) * fpb;
}

uint64_t md_next_bar_boundary(const MusicDirector* director, uint64_t from) {
    const uint64_t fpbar = md_frames_per_bar(director); 
    if (fpbar == 0) {
        return from;
    }
    return ((from + fpbar - 1) / fpbar) * fpbar;
}

void md_apply_volumes_immediate(MusicDirector* director) {
    for (std::unordered_map<std::string, MD_Stem>::iterator it = director->stems.begin(); it != director->stems.end(); ++it) {
        ae_set_volume(director->eng, it->first, it->second.currentVol);
    }
}

void md_schedule_vol(MusicDirector* director, const std::string& name, float vol, double delaySec, float fadeMs) {
    MD_Stem* st = md_get_stem(director, name); if (!st) return;
    st->startVol = st->currentVol;
    st->targetVol = md_clamp01(vol);
    st->fadeDur = (double)fadeMs / 1000.0;
    st->fadeTime = (delaySec > 0.0) ? -delaySec : 0.0;
}

void md_tick_fades(MusicDirector* director, double dtSeconds) {
    for (std::unordered_map<std::string, MD_Stem>::iterator it = director->stems.begin(); it != director->stems.end(); ++it) {
        MD_Stem& st = it->second;

        if (st.fadeTime < 0.0) {
            st.fadeTime += dtSeconds;
            if (st.fadeTime < 0.0) {
                continue;
            }
            st.fadeTime = 0.0;
        }

        if (st.fadeDur <= 0.0) {
            st.currentVol = st.targetVol;
            ae_set_volume(director->eng, it->first, st.currentVol);
            continue;
        }

        if (st.currentVol == st.targetVol) {
            continue;
        }
        st.fadeTime += dtSeconds;
        const double t = st.fadeTime / st.fadeDur;
        if (t >= 1.0) {
            st.currentVol = st.targetVol;
            ae_set_volume(director->eng, it->first, st.currentVol);
        }
        else {
            const float v = (float)((1.0 - t) * st.startVol + t * st.targetVol);
            st.currentVol = v;
            ae_set_volume(director->eng, it->first, st.currentVol);
        }
    }
}

void md_apply_state_profile(MusicDirector* director, MD_State s, bool alignToNextBar, float fadeMs) {
    float drums;
    float bass; 
    float percussion;
    float synth; 
    float lead;
    switch (s) {
        case MD_Calm:      { drums = 0.6f; bass = 0.6f; percussion = 0.f; synth = 0.f;  lead = 0.f; } break;
        case MD_Tense:     { drums = 0.9f; bass = 0.9f; percussion = 0.f; synth = 0.2f; lead = 0.f; } break;
        case MD_Combat:    { drums = 0.9f; bass = 0.9f; percussion = 0.7f; synth = 0.6f; lead = 0.7f; } break;
        case MD_Overdrive: { drums = 1.0f; bass = 1.0f; percussion = 1.0f; synth = 0.9f; lead = 1.0f; } break;
    }

    const uint64_t nowF = (uint64_t)ae_now_frames(director->eng);
    const uint64_t when = alignToNextBar ? md_next_bar_boundary(director, nowF) : nowF;
    const double delaySec = (when > nowF) ? md_frames_to_seconds(director, when - nowF) : 0.0;

    md_schedule_vol(director, "percussion", percussion, delaySec, fadeMs);
    md_schedule_vol(director, "bass", bass, delaySec, fadeMs);
    md_schedule_vol(director, "drums", drums, delaySec, fadeMs);
    md_schedule_vol(director, "synth", synth, delaySec, fadeMs);
    md_schedule_vol(director, "lead", lead, delaySec, fadeMs);
}

void md_apply_rage_shaping(MusicDirector* director, float rageVal, bool alignToBeat, float fadeMs) {
    const float bassBase = 1.0f * rageVal;
    const float percBase = 1.0f * rageVal;
    const float leadBase = (float)std::pow((double)rageVal, 1.2);

    if (bassBase > 0.0f && md_target_or_zero(director, "bass") < bassBase) {
        const uint64_t nowF = (uint64_t)ae_now_frames(director->eng);
        const uint64_t when = alignToBeat ? md_next_beat_boundary(director, nowF) : nowF;
        const double delaySec = (when > nowF) ? md_frames_to_seconds(director, when - nowF) : 0.0;
        md_schedule_vol(director, "bass", bassBase, delaySec, fadeMs);
    }
    if (percBase > 0.0f && md_target_or_zero(director, "percussion") < percBase) {
        const uint64_t nowF = (uint64_t)ae_now_frames(director->eng);
        const uint64_t when = alignToBeat ? md_next_beat_boundary(director, nowF) : nowF;
        const double delaySec = (when > nowF) ? md_frames_to_seconds(director, when - nowF) : 0.0;
        md_schedule_vol(director, "percussion", percBase, delaySec, fadeMs);
    }
    if (leadBase > 0.0f && md_target_or_zero(director, "lead") < leadBase) {
        const uint64_t nowF = (uint64_t)ae_now_frames(director->eng);
        const uint64_t when = alignToBeat ? md_next_beat_boundary(director, nowF) : nowF;
        const double delaySec = (when > nowF) ? md_frames_to_seconds(director, when - nowF) : 0.0;
        md_schedule_vol(director, "lead", leadBase, delaySec, fadeMs);
    }
}

void md_set_tunnel_vision(MusicDirector* director, float factor, bool alignToBeat, float fadeMs) {
    const double cutoff = 18000.0 - (18000.0 - 1200.0) * std::pow((double)factor, 1.6);
    ae_set_lpf_cutoff(director->eng, cutoff);

    float x = (factor - 0.60f) / 0.35f;
    if (x < 0.f) {
        x = 0.f;
    }

    if (x > 1.f) {
        x = 1.f;
    }

    const float ease = x * x * (3.f - 2.f * x);

    if (director->stems.find("rage") != director->stems.end()) {
        const float breathVol = ease;
        const float breathPitch = 0.9f + 0.4f * ease;
        md_schedule_vol(director, "rage", breathVol * 0.8f, 0.0, 200.f);
        ae_set_pitch(director->eng, "rage", breathPitch);
    }

    const float PITCH_MIN = 1.00f;
    const float PITCH_MAX = 1.10f;
    director->targetPitch = PITCH_MIN + (PITCH_MAX - PITCH_MIN) * ease;
}

bool md_init(MusicDirector* director, audio_engine* eng, const MD_Settings* s) {
    if (!director || !eng || !s) {
        return false;
    }

    *director = MusicDirector();
    director->eng = eng;
    director->cfg = *s;
    director->targetPitch = 1.0f;
    director->currentPitch = 1.0f;
    director->running = false;
    director->state = MD_Calm;
    director->rage = 0.0f;
    
    return true;
}

void md_shutdown(MusicDirector* director) {
    if (!director) {
        return;
    }

    director->stems.clear();
    director->running = false;
}

bool md_load_stems(MusicDirector* director, const std::vector<MD_StemDesc>& stems) {
    if (!director || !director->eng) {
        return false;
    }
    director->stems.reserve(director->stems.size() + stems.size());
    for (size_t i = 0; i < stems.size(); ++i) {
        const MD_StemDesc& d = stems[i];
        if (!ae_load_sound(director->eng, d.name, d.filepath, d.route, d.startActive ? 1 : 0.f, true, true))
            return false;
        MD_Stem st; st.name = d.name;
        st.currentVol = d.startActive ? 1 : 0.f;
        st.startVol = st.currentVol;
        st.targetVol = st.currentVol;
        st.fadeTime = 0.0;
        st.fadeDur = 0.0;
        director->stems[d.name] = st;
    }
    return true;
}

bool md_start_all_synced_looping(MusicDirector* director) {
    if (!director || !director->eng) {
        return false;
    }

    if (director->stems.empty()) {
        return false;
    }

    const uint64_t syncTime = (uint64_t)ae_now_frames(director->eng) + md_seconds_to_frames(director, director->cfg.initialStartDelaySec);
    ae_start_all_at(director->eng, syncTime, true);
    director->running = true;
    md_apply_volumes_immediate(director);
    return true;
}

void md_update(MusicDirector* director, double dtSeconds) {
    if (!director || !director->running) {
        return;
    }

    md_tick_fades(director, dtSeconds);

    double a = dtSeconds * 4.0; if (a > 1.0) a = 1.0;
    const float prev = director->currentPitch;
    director->currentPitch = (float)((1.0 - a) * director->currentPitch + a * director->targetPitch);
    if (std::fabs(director->currentPitch - prev) > 1e-3f) {
        ae_set_all_pitch(director->eng, director->currentPitch, "rage");
    }
}

void md_set_state(MusicDirector* director, MD_State s, bool alignToNextBar, float fadeMs) {
    if (!director) {
        return;
    }

    director->state = s;
    md_apply_state_profile(director, s, alignToNextBar, fadeMs);
}

void md_set_rage(MusicDirector* director, float r01, bool alignToNextBeat, float fadeMs) {
    if (!director) {
        return;
    }

    if (r01 < 0.f) {
        r01 = 0.f;
    }

    if (r01 > 1.f) {
        r01 = 1.f;
    }

    director->rage = r01;

    md_set_tunnel_vision(director, r01, alignToNextBeat, fadeMs);
}

void md_set_stem_target_volume(MusicDirector* director, const std::string& name, float volume,
    bool alignToBeat, float fadeMs) {
    if (!director) {
        return;
    }
    MD_Stem* st = md_get_stem(director, name);
    if (!st) {
        return;
    }

    if (volume < 0.f) {
        volume = 0.f;
    }

    if (volume > 1.f) {
        volume = 1.f;
    }

    const uint64_t nowF = (std::uint64_t)ae_now_frames(director->eng);
    const uint64_t startWhen = alignToBeat ? md_next_beat_boundary(director, nowF) : nowF;
    const double delaySec = (startWhen > nowF) ? md_frames_to_seconds(director, startWhen - nowF) : 0.0;

    st->startVol = st->currentVol;
    st->targetVol = volume;
    st->fadeDur = (double)fadeMs / 1000.0;
    st->fadeTime = (delaySec > 0.0) ? -delaySec : 0.0;
}

float md_get_bpm(const MusicDirector* director) { return director ? director->cfg.bpm : 0.f; }
void  md_set_bpm(MusicDirector* director, float bpm) { if (director) director->cfg.bpm = bpm; }
MD_State md_get_state(const MusicDirector* director) { return director ? director->state : MD_Calm; }

float md_get_stem_current_volume(const MusicDirector* director, const std::string& name) {
    if (!director) {
        return 0.f;
    }
    std::unordered_map<std::string, MD_Stem>::const_iterator it = director->stems.find(name);
    return (it == director->stems.end()) ? 0.f : it->second.currentVol;
}

void md_music_clock(const MusicDirector* director, float* outBeatPhase, float* outBarPhase) {
    if (!director || !director->eng) { 
        if (outBeatPhase) {
            *outBeatPhase = 0.f;
        }
        if (outBarPhase) {
            *outBarPhase = 0.f;
        }
        return; 
    }
    const uint64_t nowF = (uint64_t)ae_now_frames(director->eng);
    const uint64_t fpBeat = md_frames_per_beat(director);
    const uint64_t fpBar = md_frames_per_bar(director);
    if (outBeatPhase) {
        *outBeatPhase = fpBeat ? float(nowF % fpBeat) / float(fpBeat) : 0.f;
    }

    if (outBarPhase) {
        *outBarPhase = fpBar ? float(nowF % fpBar) / float(fpBar) : 0.f;
    }
}