#include <unordered_map>
#include <string>
#include <cmath>
#include <cstdint>
#define MA_ENABLE_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "../Include/miniaudio.h"

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

float ae_clamp01(float v) { return (v < 0.f) ? 0.f : ((v > 1.f) ? 1.f : v); }

audio_sound* ae_find(audio_engine* e, const std::string& name) {
    std::unordered_map<std::string, audio_sound>::iterator it = e->sounds.find(name);
    if (it == e->sounds.end()) {
        return (audio_sound*)0;
    }
    return &it->second;
}

float ae_apply_route(float baseVol, audio_route r, const audio_engine* e) {
    int idx = (r == MAE_BypassLPF) ? 1 : 0;
    float g = e ? e->routeGain[idx] : 1.f;
    float v = baseVol * g;
    return (v < 0.f) ? 0.f : (v > 1.f ? 1.f : v);
}


bool ae_init(audio_engine* engine, const audio_config* cfg) {
    if (!engine || !cfg) {
        return false;
    }

    *engine = audio_engine{};
    engine->cfg = *cfg;

    ma_engine_config engCfg = ma_engine_config_init();
    if (cfg->sampleRate > 0) {
        engCfg.sampleRate = (ma_uint32)cfg->sampleRate;
    }

    if (cfg->channels > 0) {
        engCfg.channels = (ma_uint32)cfg->channels;
    }

    if (ma_engine_init(&engCfg, &engine->engine) != MA_SUCCESS) {
        return false;
    }

    engine->engineInit = true;
    engine->sampleRate = ma_engine_get_sample_rate(&engine->engine);
    engine->channels = ma_engine_get_channels(&engine->engine);

    ma_hpf_node_config hyPassCfg = ma_hpf_node_config_init(engine->channels, engine->sampleRate, 20.0, /*order*/2);
    if (ma_hpf_node_init(ma_engine_get_node_graph(&engine->engine), &hyPassCfg, nullptr, &engine->hpf) != MA_SUCCESS) {
        ma_engine_uninit(&engine->engine); engine->engineInit = false; return false;
    }
    engine->hpfInit = true;

    ma_lpf_node_config lowPassCfg = ma_lpf_node_config_init(engine->channels, engine->sampleRate, engine->cfg.lpfStartHz, engine->cfg.lpfOrder);
    if (ma_lpf_node_init(ma_engine_get_node_graph(&engine->engine), &lowPassCfg, nullptr, &engine->lpf) != MA_SUCCESS) {
        ma_hpf_node_uninit(&engine->hpf, nullptr); engine->hpfInit = false;
        ma_engine_uninit(&engine->engine); engine->engineInit = false; return false;
    }
    engine->lpfInit = true;

    ma_node_attach_output_bus(&engine->hpf, 0, (ma_node*)&engine->lpf, 0);
    ma_node_attach_output_bus(&engine->lpf, 0, ma_engine_get_endpoint(&engine->engine), 0);
    return true;
}

void ae_shutdown(audio_engine* engine) {
    if (!engine) {
        return;
    }

    std::unordered_map<std::string, audio_sound>::iterator it;
    for (it = engine->sounds.begin(); it != engine->sounds.end(); ++it) {
        if (it->second.loaded) {
            ma_sound_uninit(&it->second.sound);
            it->second.loaded = false;
        }
    }
    engine->sounds.clear();

    if (engine->lpfInit) { ma_lpf_node_uninit(&engine->lpf, nullptr); engine->lpfInit = false; }
    if (engine->hpfInit) { ma_hpf_node_uninit(&engine->hpf, nullptr); engine->hpfInit = false; }
    if (engine->engineInit) { ma_engine_uninit(&engine->engine); engine->engineInit = false; }
}

bool ae_load_sound(audio_engine* engine, const std::string& name, const std::string& path, audio_route route, float initVolume = 1.0f, bool loop = true, bool stream = true)
{
    if (!engine) {
        return false;
    }

    audio_sound& sound = engine->sounds[name];
    if (sound.loaded) { 
        ma_sound_uninit(&sound.sound); 
        sound.loaded = false; 
    }

    ma_uint32 flags = MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
    if (stream) flags |= MA_SOUND_FLAG_STREAM;

    if (ma_sound_init_from_file(&engine->engine, path.c_str(), flags, nullptr, nullptr, &sound.sound) != MA_SUCCESS) {
        engine->sounds.erase(name);
        return false;
    }

    sound.loaded = true;
    sound.route = route;
    sound.baseVol = ae_clamp01(initVolume);

    ma_sound_set_looping(&sound.sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(&sound.sound, ae_apply_route(sound.baseVol, sound.route, engine));

    if (route == MAE_ThroughLPF) {
        ma_node_attach_output_bus((ma_node*)&sound.sound, 0, (ma_node*)&engine->hpf, 0);
    }
    else {
        ma_node_attach_output_bus((ma_node*)&sound.sound, 0, ma_engine_get_endpoint(&engine->engine), 0);
    }
    return true;
}

void ae_start(audio_engine* engine, const std::string& name) {
    if (!engine) {
        return;
    }

    audio_sound* sound = ae_find(engine, name);
    if (sound) {
        ma_sound_start(&sound->sound);
    }
}

void ae_stop(audio_engine* engine, const std::string& name) {
    if (!engine) {
        return;
    }
    audio_sound* sound = ae_find(engine, name);
    if (sound) {
        ma_sound_stop(&sound->sound);
    }
}

void ae_start_all_at(audio_engine* engine, ma_uint64 t0Frames, bool seekZero = true) {
    if (!engine) {
        return;
    }
    std::unordered_map<std::string, audio_sound>::iterator it;
    for (it = engine->sounds.begin(); it != engine->sounds.end(); ++it) {
        audio_sound& sound = it->second;
        if (!sound.loaded) continue;
        if (seekZero) ma_sound_seek_to_pcm_frame(&sound.sound, 0);
        ma_sound_set_start_time_in_pcm_frames(&sound.sound, t0Frames);
        ma_sound_start(&sound.sound);
    }
}

void ae_reroute(audio_engine* engine, const std::string& name, audio_route newRoute) {
    if (!engine) {
        return;
    }

    audio_sound* sound = ae_find(engine, name);
    if (!sound) {
        return;
    }
    sound->route = newRoute;
    if (newRoute == MAE_ThroughLPF) {
        ma_node_attach_output_bus((ma_node*)&sound->sound, 0, (ma_node*)&engine->hpf, 0);
    } else {
        ma_node_attach_output_bus((ma_node*)&sound->sound, 0, ma_engine_get_endpoint(&engine->engine), 0);
    }
    ma_sound_set_volume(&sound->sound, ae_apply_route(sound->baseVol, sound->route, engine));
}

void ae_set_volume(audio_engine* engine, const std::string& name, float vol01) {
    if (!engine) {
        return;
    }

    audio_sound* sound = ae_find(engine, name);
    if (!sound) {
        return;
    }
    sound->baseVol = ae_clamp01(vol01);
    ma_sound_set_volume(&sound->sound, ae_apply_route(sound->baseVol, sound->route, engine));
}

void ae_set_pitch(audio_engine* engine, const std::string& name, float pitch) {
    if (!engine) {
        return;
    }

    if (pitch < 0.01f) {
        pitch = 0.01f;
    }

    audio_sound* sound = ae_find(engine, name); 
    if (!sound) {
        return;
    }
    ma_sound_set_pitch(&sound->sound, pitch);
}

void ae_set_pan(audio_engine* engine, const std::string& name, float pan_m1_p1) {
    if (!engine) {
        return;
    }

    if (pan_m1_p1 < -1.f) {
        pan_m1_p1 = -1.f;
    }

    if (pan_m1_p1 > 1.f) {
        pan_m1_p1 = 1.f;
    }

    audio_sound* sound = ae_find(engine, name); 
    if (!sound) {
        return;
    }
    ma_sound_set_pan(&sound->sound, pan_m1_p1);
}

void ae_set_all_pitch(audio_engine* engine, float pitch, const std::string& except = std::string()) {
    if (!engine) {
        return;
    }

    if (pitch < 0.01f) {
        pitch = 0.01f;
    }

    std::unordered_map<std::string, audio_sound>::iterator it;
    for (it = engine->sounds.begin(); it != engine->sounds.end(); ++it) {
        if (!it->second.loaded) {
            continue;
        }
        if (!except.empty() && it->first == except) {
            continue;
        }
        ma_sound_set_pitch(&it->second.sound, pitch);
    }
}

void ae_set_route_gain(audio_engine* engine, audio_route route, float gain01) {
    if (!engine) {
        return;
    }
    int idx = (route == MAE_BypassLPF) ? 1 : 0;
    engine->routeGain[idx] = ae_clamp01(gain01);
    std::unordered_map<std::string, audio_sound>::iterator it;
    for (it = engine->sounds.begin(); it != engine->sounds.end(); ++it) {
        audio_sound& sound = it->second;
        if (!sound.loaded || sound.route != route) {
            continue;
        }
        ma_sound_set_volume(&sound.sound, ae_apply_route(sound.baseVol, sound.route, engine));
    }
}

void ae_set_hpf_cutoff(audio_engine* engine, double cutoffHz) {
    if (!engine || !engine->hpfInit) {
        return;
    }
    ma_hpf_config cfg = ma_hpf_config_init(ma_format_f32, engine->channels, engine->sampleRate, cutoffHz, 2);
    ma_hpf_node_reinit(&cfg, &engine->hpf);
}

void ae_set_lpf_cutoff(audio_engine* engine, double cutoffHz) {
    if (!engine || !engine->lpfInit) {
        return;
    }
    ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, engine->channels, engine->sampleRate, cutoffHz, engine->cfg.lpfOrder);
    ma_lpf_node_reinit(&cfg, &engine->lpf);
}

ma_uint64 ae_now_frames(const audio_engine* engine) {
    return ma_engine_get_time_in_pcm_frames(const_cast<ma_engine*>(&engine->engine));
}
ma_uint64 ae_seconds_to_frames(const audio_engine* engine, double seconds) {
    return (ma_uint64)llround(seconds * (double)engine->sampleRate);
}
double ae_frames_to_seconds(const audio_engine* engine, ma_uint64 frames) {
    return (double)frames / (double)engine->sampleRate;
}

ma_uint32 ae_sample_rate(const audio_engine* engine) { return engine ? engine->sampleRate : 0; }
ma_uint32 ae_channels(const audio_engine* engine) { return engine ? engine->channels : 0; }