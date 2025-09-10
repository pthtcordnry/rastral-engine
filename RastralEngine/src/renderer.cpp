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

GLuint g_uboPerFrame = 0;
GLuint g_uboPerDraw = 0;
GLuint g_uboViz = 0;

void CreateUBOs()
{
    if (!g_uboPerFrame) { 
        glGenBuffers(1, &g_uboPerFrame);
    }
    
    if (!g_uboPerDraw) { 
        glGenBuffers(1, &g_uboPerDraw);
    }
    
    if (!g_uboViz) {
        glGenBuffers(1, &g_uboViz); 
    }

    glBindBuffer(GL_UNIFORM_BUFFER, g_uboPerFrame);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(PerFrameUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_uboPerFrame);

    glBindBuffer(GL_UNIFORM_BUFFER, g_uboPerDraw);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(PerDrawUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_uboPerDraw);

    glBindBuffer(GL_UNIFORM_BUFFER, g_uboViz);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(VizParamsUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, g_uboViz);

    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void DestroyUBOs()
{
    if (g_uboPerFrame) { 
        glDeleteBuffers(1, &g_uboPerFrame); 
        g_uboPerFrame = 0; 
    }
    if (g_uboPerDraw) { 
        glDeleteBuffers(1, &g_uboPerDraw);  
        g_uboPerDraw = 0; 
    }
    if (g_uboViz) { 
        glDeleteBuffers(1, &g_uboViz);
        g_uboViz = 0; 
    }
}

void BindUBOsForMesh(GLuint program)
{
    GLuint idx;

    idx = glGetUniformBlockIndex(program, "PerFrame");
    if (idx != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, idx, 0);
    }

    idx = glGetUniformBlockIndex(program, "PerDraw");
    if (idx != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, idx, 1);
    }
}

void BindUBOsForVisualizer(GLuint program)
{
    GLuint idx = glGetUniformBlockIndex(program, "VizParams");
    if (idx != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, idx, 2);
    }
}


void UpdatePerFrameUBO(const float projView16[16])
{
    PerFrameUBO data;
    memcpy(data.uProjView, projView16, 16 * sizeof(float));
    glBindBuffer(GL_UNIFORM_BUFFER, g_uboPerFrame);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerFrameUBO), &data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void UpdatePerDrawUBO(const float model16[16], const float tint4[4])
{
    PerDrawUBO data;
    memcpy(data.uModel, model16, 16 * sizeof(float));
    memcpy(data.uTint, tint4, 4 * sizeof(float));
    glBindBuffer(GL_UNIFORM_BUFFER, g_uboPerDraw);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerDrawUBO), &data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void UpdateVizParamsUBO(float resX, float resY, float time, float beatPhase, float barPhase, int state,
                    float rage, float drums, float bass, float perc, float synth, float levelLead)
{
    VizParamsUBO v = {};
    v.uRes[0] = resX;  v.uRes[1] = resY;
    v.uTime = time;
    v.uBeatPhase = beatPhase;
    v.uBarPhase = barPhase;
    v.uState = state;
    v.uRage = rage;
    v.uLevelsA[0] = drums; v.uLevelsA[1] = bass; v.uLevelsA[2] = perc; v.uLevelsA[3] = synth;
    v.uLevelLead = levelLead;

    glBindBuffer(GL_UNIFORM_BUFFER, g_uboViz);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(VizParamsUBO), &v);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}