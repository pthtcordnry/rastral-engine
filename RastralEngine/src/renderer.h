#ifndef RENDERER_H
#define RENDERER_H

#include <cstring>

struct PerFrameUBO {
    float uProjView[16];
};
struct PerDrawUBO {
    float uModel[16];
    float uTint[4];
};

struct VizParamsUBO {
    float uRes[2];
    float uTime;
    float _pad0;
    float uBeatPhase;
    float uBarPhase;
    int uState;
    float uRage;
    float uLevelsA[4];
    float uLevelLead;
    float _pad2[3];
};

struct RenderTarget {
    unsigned int fbo;
    unsigned int color;
    unsigned int depth;
    int    w;
    int    h;
};

int g_view_w = 1280;
int g_view_h = 720;

unsigned int g_uboPerFrame = 0;
unsigned int g_uboPerDraw = 0;
unsigned int g_uboViz = 0;

#endif