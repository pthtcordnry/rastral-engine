#ifndef RENDERER_H
#define RENDERER_H

#include <cstring>

// ---------------- UBO layouts ----------------
struct PerFrameUBO {
    float uProjView[16];
};
struct PerDrawUBO {
    float uModel[16];
    float uTint[4];
};

// Keep your visualizer UBO as-is
struct VizParamsUBO {
    float uRes[2];
    float uTime;
    float _pad0;
    float uBeatPhase;
    float uBarPhase;
    int   uState;
    float uRage;
    float uLevelsA[4];
    float uLevelLead;
    float _pad2[3];
};

// NEW: Skin palette UBO (binding = 3). 128 bones max is typical.
struct SkinUBO {
    float uBones[128][16]; // row-major 4x4 per bone
    int   uBoneCount;      // optional, not required by shader
    int   _pad[3];
};

// ---------------- RenderTarget ----------------
struct RenderTarget {
    unsigned int fbo;
    unsigned int color;
    unsigned int depth;
    int    w;
    int    h;
};

// ---------------- Globals ----------------
int g_view_w = 1280;
int g_view_h = 720;

unsigned int g_uboPerFrame = 0;
unsigned int g_uboPerDraw = 0;
unsigned int g_uboViz = 0;
unsigned int g_uboSkin = 0;  // NEW

#endif
