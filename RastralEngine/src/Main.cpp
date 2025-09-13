#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <GL/glew.h>
#include <GL/wglew.h>
#include <cfloat>

#include "renderer.h"
#include "opengl_renderer.cpp"
#include "gltf_loader.cpp"
#include "MusicDirector.cpp"
#include "windows_input.cpp"
#include "engine_data.cpp"
#include "memory_arena.cpp"

struct Win32Window {
    HINSTANCE hinst;
    HWND hwnd;
    HDC hdc;
    int width;
    int height;
    bool running;
};

struct GLContext {
    HGLRC rc;
    bool core;
};

struct HiResTimer {
    LARGE_INTEGER f;
    LARGE_INTEGER t0;
};

Win32Window g_win = {};
GLContext   g_gl = {};
HiResTimer  g_tim = {};

EngineData* engineData;
RenderState* renderState;

std::string gMeshShaderBase = "shaders/simple_uv";
std::string gPostShaderBase = "shaders/visualizer";

GLuint gTex_Albedo = 0;
GLenum gMeshIndexType = GL_UNSIGNED_INT;
Mat4 gModelPreXform = matIdentity();

// ---------- Animation externs from gltf_loader.cpp ----------
namespace tinygltf { class Model; }
extern const tinygltf::Model& GLTF_GetModel();
extern void GLTF_UpdateAnimation_Pose(const tinygltf::Model& model, float tSec);
struct GLTFDraw; // already defined in gltf_loader.cpp
extern std::vector<GLTFDraw> gGLTFDraws;
extern void GLTF_GetBonesForDraw(const GLTFDraw& d, std::vector<float>& out16);
extern float gModelFitRadius; // from loader
extern bool  gPlaceOnGround;  // from loader

float NowSecs(const HiResTimer& t) {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return float(double(n.QuadPart - t.t0.QuadPart) / double(t.f.QuadPart));
}

void TimerStart(HiResTimer& t) {
    QueryPerformanceFrequency(&t.f);
    QueryPerformanceCounter(&t.t0);
}

void Win32Fail(const char* where) {
    DWORD error = GetLastError();
    char msg[1024]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, msg, sizeof(msg), nullptr);
    MessageBoxA(nullptr, msg, where, MB_ICONERROR);
}

bool Win32SetBasicPixelFormat(HDC hdc) {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int fmt = ChoosePixelFormat(hdc, &pfd);

    if (!fmt) {
        Win32Fail("ChoosePixelFormat");
        return false;
    }

    if (!SetPixelFormat(hdc, fmt, &pfd)) {
        Win32Fail("SetPixelFormat");
        return false;
    }

    return true;
}

bool GLCreateContextPreferCore(HDC hdc, GLContext& c) {
    HGLRC temp = wglCreateContext(hdc);
    if (!temp) {
        Win32Fail("wglCreateContext(temp)");
        return false;
    }
    if (!wglMakeCurrent(hdc, temp)) {
        Win32Fail("wglMakeCurrent(temp)");
        return false;
    }
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        c.rc = temp;
        c.core = false;
        return true;
    }
    if (wglCreateContextAttribsARB) {
        const int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB,3, WGL_CONTEXT_MINOR_VERSION_ARB,3,
                                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        HGLRC core = wglCreateContextAttribsARB(hdc, 0, attribs);
        if (core) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(temp);
            if (!wglMakeCurrent(hdc, core)) {
                wglDeleteContext(core);
                c.rc = wglCreateContext(hdc);
                wglMakeCurrent(hdc, c.rc);
                c.core = false;
            }
            else {
                glewExperimental = GL_TRUE;
                if (glewInit() != GLEW_OK) {
                    Win32Fail("glewInit(core)");
                    return false;
                }
                c.rc = core;
                c.core = true;
            }
            return true;
        }
    }

    c.rc = temp;
    c.core = false;
    return true;
}

bool PumpMessages(Win32Window& w) {
    MSG msg{};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            w.running = false;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return w.running;
}

void SetSwapInterval(int interval) {
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(interval);
    }
}

void UpdateWindowTitle() {
    if (!g_win.hwnd) {
        return;
    }

    char title[256];
    snprintf(title, sizeof(title),
        "Rastral Engine | state=%s rage=%.2f vsync=%s | scale=%.3f dist=%.2f",
        StateName(md_get_state(&engineData->g_md)), engineData->g_rage, engineData->g_vsyncOn ? "on" : "off",
        renderState->gUserScale, renderState->gCamDist);
    SetWindowTextA(g_win.hwnd, title);
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

void LoadShaders_FromFiles() {
    const std::string vsMesh = ReadTextFile(gMeshShaderBase + ".vert");
    const std::string fsMesh = ReadTextFile(gMeshShaderBase + ".frag");
    const std::string vsPost = ReadTextFile(gPostShaderBase + ".vert");
    const std::string fsPost = ReadTextFile(gPostShaderBase + ".frag");
    if (vsMesh.empty() || fsMesh.empty() || vsPost.empty() || fsPost.empty()) {
        MessageBoxA(nullptr, "Missing shader source files.", "Shader Error", MB_ICONERROR);
        ExitProcess(1);
    }
    DestroyProgram(renderState->gProgramMesh);
    DestroyProgram(renderState->gProgramPost);
    renderState->gProgramMesh = CreateProgramFromSources(vsMesh.c_str(), fsMesh.c_str());
    renderState->gProgramPost = CreateProgramFromSources(vsPost.c_str(), fsPost.c_str());
    if (!renderState->gProgramMesh || !renderState->gProgramPost) {
        MessageBoxA(nullptr, "Shader compile/link failed.", "Shader Error", MB_ICONERROR);
        ExitProcess(1);
    }
    InitMeshProgram(renderState->gProgramMesh);
    InitPostProgram(renderState->gProgramPost);
}

void InitData() {
    arena_init(&engineMemArena, GAME_ARENA_SIZE);
    void* p1 = arena_alloc(&engineMemArena, sizeof(EngineData));
    void* p2 = arena_alloc(&engineMemArena, sizeof(RenderState));
    engineData = new (p1) EngineData();
    renderState = new (p2) RenderState();
}

bool InitAudio() {
    audio_config engCfg = { 0,0,18000.0,4 };

    if (!ae_init(&engineData->g_audio, &engCfg)) {
        MessageBoxA(nullptr, "Audio init failed to initialize.", "Error", MB_ICONERROR);
        return false;
    }

    MD_Settings s{};
    s.bpm = 110.f;
    s.initialStartDelaySec = 0.15f;
    if (!md_init(&engineData->g_md, &engineData->g_audio, &s)) {
        MessageBoxA(nullptr, "Audio init failed to initialzed MusicDirector.", "Program Load Step", MB_ICONINFORMATION);
    }

    std::vector<MD_StemDesc> stems = {
        {"drums","audio/drums.flac",true,MAE_ThroughLPF},
        {"bass","audio/bass.flac",false,MAE_ThroughLPF},
        {"percussion","audio/percussion.flac",false,MAE_ThroughLPF},
        {"synth","audio/synth.flac",false,MAE_ThroughLPF},
        {"lead","audio/synth_lead.flac",false,MAE_ThroughLPF},
    };

    if (!md_load_stems(&engineData->g_md, stems)) {
        MessageBoxA(nullptr, "Audio init failed to load stems.", "Error", MB_ICONERROR);
        return false;
    }

    if (!md_start_all_synced_looping(&engineData->g_md)) {
        MessageBoxA(nullptr, "Audio init failed to start loop.", "Error", MB_ICONERROR);
        return false;
    }

    md_set_state(&engineData->g_md, MD_Calm, false, 0.0f);
    engineData->g_audioReady = true;
    return true;
}

void ShutdownAudio() {
    if (!engineData->g_audioReady) {
        return;
    }
    md_shutdown(&engineData->g_md);
    ae_shutdown(engineData->g_md.eng);
    engineData->g_audioReady = false;
}


void InitGraphics(int width, int height) {
    SetViewportSize(width, height);
    LoadShaders_FromFiles();

    // NOTE: keeps your existing loader signature exactly as-is.
    if (!CreateMeshFromGLTF_PosUV_Textured("models/idle-bot.glb", renderState->gVAO_Mesh, renderState->gVBO_Mesh, renderState->gEBO_Mesh, gModelPreXform)) {
        MessageBoxA(nullptr, "Failed to load models/idle-bot.glb", "glTF Load Error", MB_ICONERROR);
    }

    CreateFullscreenQuad(&renderState->gVAO_Post, &renderState->gVBO_Post);
    CreateRenderTarget(renderState->gRT_Scene, g_view_w, g_view_h);
    CreateUBOs();

    if (!gTex_Albedo) {
        unsigned char white[4] = { 255,255,255,255 };
        gTex_Albedo = CreateTexture2D(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, white, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    renderState->gYaw = DegToRad(35.0f);
    renderState->gPitch = DegToRad(20.0f);

    float aspect = (float)g_view_w / (float)g_view_h;
    float vfov = DegToRad(60.0f);
    renderState->gCamDist = DistanceToFitSphere(gModelFitRadius, vfov, aspect);
}

void RenderFrame(float tSeconds, int viewW, int viewH) {
    SetViewportSize(viewW, viewH);

    BeginRenderTarget(renderState->gRT_Scene);
    BeginFrame(0.05f, 0.06f, 0.08f, 1.0f);

    float aspect = (float)g_view_w / (float)g_view_h;
    Mat4 P = matPerspective(60.0f * 3.1415926f / 180.0f, aspect, 0.05f, 1000.0f);

    const float cp = std::cos(renderState->gPitch), sp = std::sin(renderState->gPitch);
    const float cy = std::cos(renderState->gYaw), sy = std::sin(renderState->gYaw);

    // orbit around the model target point
    const float cx = gModelTarget[0];
    const float cyT = gModelTarget[1];
    const float cz = gModelTarget[2];

    const float dist = renderState->gCamDist;
    float eyeX = cx + dist * cp * sy;
    float eyeY = cyT + dist * sp;
    float eyeZ = cz + dist * cp * cy;

    Mat4 V = matLookAt(eyeX, eyeY, eyeZ, cx, cyT, cz, 0.0f, 1.0f, 0.0f);
    Mat4 PV = matMul(P, V);
    UpdatePerFrameUBO(PV.m);

    // Drive animation (idle) -> fills gGlobalsAnimated
    GLTF_UpdateAnimation_Pose(GLTF_GetModel(), tSeconds);

    BeginShader(renderState->gProgramMesh);
    BindVAO(renderState->gVAO_Mesh);

    const Mat4 GlobalPre = gModelPreXform;
    std::vector<float> animBones;

    for (const auto& d : gGLTFDraws) {
        // For skinned draws, glTF needs the mesh node’s world matrix too.
        // uModel = GlobalPre * nodeWorld   (skinned)
        // uModel = GlobalPre               (static; WM already baked into vertices)
        Mat4 Mdraw = d.skinned ? matMul(GlobalPre, d.localModel) : GlobalPre;

        const float* bones = nullptr;
        if (d.boneCount > 0) {
            GLTF_GetBonesForDraw(d, animBones);   // yields (jointWorld * inverseBind)
            bones = animBones.data();
        }

        UpdatePerDrawUBO(Mdraw.m, d.baseColor);
        UpdateSkinUBO(bones, d.boneCount);

        BindTexture2D(0, d.texture ? d.texture : gTex_Albedo);
        DrawIndexedTriangles(d.indexCount, (void*)(d.indexOffset * sizeof(uint32_t)));
    }

    BindVAO(0);
    BindTexture2D(0, 0);
    EndShader();

    EndRenderTarget();

    // --- post pass (unchanged) ---
    float beatPhase = 0.f, barPhase = 0.f; md_music_clock(&engineData->g_md, &beatPhase, &barPhase);
    float vDrums = md_get_stem_current_volume(&engineData->g_md, "drums");
    float vBass = md_get_stem_current_volume(&engineData->g_md, "bass");
    float vPerc = md_get_stem_current_volume(&engineData->g_md, "percussion");
    float vSynth = md_get_stem_current_volume(&engineData->g_md, "synth");
    float vLead = md_get_stem_current_volume(&engineData->g_md, "lead");
    int   stateI = (int)md_get_state(&engineData->g_md);

    BeginFrame(0, 0, 0, 1);
    BeginShader(renderState->gProgramPost);
    UpdateVizParamsUBO((float)g_view_w, (float)g_view_h, tSeconds, beatPhase, barPhase, stateI, engineData->g_rage,
        vDrums, vBass, vPerc, vSynth, vLead);
    BindTexture2D(0, renderState->gRT_Scene.color);
    BindVAO(renderState->gVAO_Post);
    DrawTriangles(0, 6);
    BindVAO(0);
    BindTexture2D(0, 0);
    EndShader();
    EndFrame();
}

void HandleInput() {
    const float step = 0.05f;
    const float scaleStep = 1.10f;
    const float distStep = 0.25f;
    const float yawStep = 0.02f;
    const float pitchStep = 0.02f;

    const float kPitchMin = -1.553343f;
    const float kPitchMax = +1.553343f;

    if (Input_IsDown(VK_LEFT)) {
        renderState->gYaw -= yawStep;
    }

    if (Input_IsDown(VK_RIGHT)) {
        renderState->gYaw += yawStep;
    }

    if (Input_IsDown(VK_UP)) {
        renderState->gPitch += pitchStep;
        if (renderState->gPitch > kPitchMax) {
            renderState->gPitch = kPitchMax;
        }
    }

    if (Input_IsDown(VK_DOWN)) {
        renderState->gPitch -= pitchStep;
        if (renderState->gPitch < kPitchMin) {
            renderState->gPitch = kPitchMin;
        }
    }

    if (Input_IsDown('W')) {
        renderState->gCamDist -= distStep;
        if (renderState->gCamDist < 0.2f) {
            renderState->gCamDist = 0.2f;
        }
    }

    if (Input_IsDown('S')) {
        renderState->gCamDist += distStep;
    }

    if (Input_IsPressed('Z')) {
        renderState->gUserScale = (renderState->gUserScale / scaleStep);
        if (renderState->gUserScale < 0.0001f) {
            renderState->gUserScale = 0.0001f;
        }
    }

    if (Input_IsPressed('X')) {
        renderState->gUserScale = (renderState->gUserScale * scaleStep);
        if (renderState->gUserScale > 10000.0f) {
            renderState->gUserScale = 10000.0f;
        }
    }

    if (Input_IsPressed(VK_SPACE)) {
        float aspect = (float)g_view_w / (float)g_view_h;
        float vfov = DegToRad(60.0f);
        renderState->gCamDist = DistanceToFitSphere(gModelFitRadius, vfov, aspect);
        renderState->gYaw = DegToRad(35.0f);
        renderState->gPitch = DegToRad(-20.0f);
    }

    if (Input_IsPressed('R')) {
        renderState->gUserScale = 1.0f;
        renderState->gCamDist = 3.0f;
        renderState->gYaw = 0.0f;
        renderState->gPitch = 0.0f;
    }

    if (Input_IsPressed('1')) {
        md_set_state(&engineData->g_md, MD_Calm, true, 300.0f);
    }

    if (Input_IsPressed('2')) {
        md_set_state(&engineData->g_md, MD_Tense, true, 300.0f);
    }

    if (Input_IsPressed('3')) {
        md_set_state(&engineData->g_md, MD_Combat, true, 350.0f);
    }

    if (Input_IsPressed('4')) {
        md_set_state(&engineData->g_md, MD_Overdrive, true, 400.0f);
    }

    if (Input_IsPressed(VK_OEM_MINUS)) {
        engineData->g_rage = (engineData->g_rage - step < 0.0f) ? 0.0f : (engineData->g_rage - step);
    }

    if (Input_IsPressed(VK_SUBTRACT)) {
        engineData->g_rage = (engineData->g_rage - step < 0.0f) ? 0.0f : (engineData->g_rage - step);
    }

    if (Input_IsPressed(VK_OEM_PLUS)) {
        engineData->g_rage = (engineData->g_rage + step > 1.0f) ? 1.0f : (engineData->g_rage + step);
    }

    if (Input_IsPressed(VK_ADD)) {
        engineData->g_rage = (engineData->g_rage + step > 1.0f) ? 1.0f : (engineData->g_rage + step);
    }

    if (Input_IsPressed('Q')) {
        PostMessageA(g_win.hwnd, WM_CLOSE, 0, 0);
    }

    if (Input_IsPressed(VK_ESCAPE)) {
        PostMessageA(g_win.hwnd, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK WndProcHook(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: {
        g_win.width = int(LOWORD(l));
        g_win.height = int(HIWORD(l));
        if (g_gl.rc) {
            SetViewportSize(g_win.width, g_win.height);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
        Input_HandleKeyMsg(m, w, l);
        return 0;
    }

    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        Input_HandleKeyMsg(m, w, l);
        return DefWindowProc(h, m, w, l);
    }

    case WM_ACTIVATEAPP: {
        if (w == FALSE) {
            Input_ClearAll();
        }
        return 0;
    }

    case WM_CLOSE: {
        DestroyWindow(h);
        return 0;
    }

    case WM_DESTROY: {
        g_win.running = false;
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(h, m, w, l);
}


bool Win32CreateWindowSimple(Win32Window& w) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProcHook;
    wc.hInstance = w.hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RastralEngine";

    if (!RegisterClassExA(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Win32Fail("RegisterClassExA");
            return false;
        }
    }

    RECT r{ 0,0,w.width,w.height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    w.hwnd = CreateWindowExA(0, wc.lpszClassName, "Rastral Engine", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, w.hinst, nullptr);
    if (!w.hwnd) {
        Win32Fail("CreateWindowExA");
        return false;
    }

    w.hdc = GetDC(w.hwnd);
    if (!w.hdc) {
        Win32Fail("GetDC");
        return false;
    }

    ShowWindow(w.hwnd, SW_SHOW);
    UpdateWindow(w.hwnd);
    return true;
}

extern "C" int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_win.hinst = hInst;
    g_win.width = 1280;
    g_win.height = 720;
    g_win.running = true;

    if (!Win32CreateWindowSimple(g_win)) {
        return 1;
    }

    if (!Win32SetBasicPixelFormat(g_win.hdc)) {
        return 1;
    }

    if (!GLCreateContextPreferCore(g_win.hdc, g_gl)) {
        return 1;
    }

    SetSwapInterval(1);

    InitData();
    if (!InitAudio()) {
        MessageBoxA(nullptr, "Audio init failed.", "Error", MB_ICONERROR);
        return 2;
    }


    InitGraphics(g_win.width, g_win.height);

    Input_Init();
    UpdateWindowTitle();

    TimerStart(g_tim);
    float lastT = NowSecs(g_tim);
    bool running = true;
    while (running) {
        Input_BeginFrame();
        running = PumpMessages(g_win);
        HandleInput();

        float now = NowSecs(g_tim);
        float dt = now - lastT; lastT = now;

        if (engineData->g_audioReady) {
            md_set_rage(&engineData->g_md, engineData->g_rage, true, 180.0f);
            md_update(&engineData->g_md, dt);
        }

        RenderFrame(now, g_win.width, g_win.height);
        SwapBuffers(g_win.hdc);
        if (!engineData->g_vsyncOn) {
            using namespace std::chrono;
            static auto last = high_resolution_clock::now();
            const auto target = milliseconds(16);
            auto now = high_resolution_clock::now();
            auto elapsed = now - last;
            if (elapsed < target) {
                std::this_thread::sleep_for(target - elapsed);
            }
            last = now;
        }
    }

    if (renderState->gProgramMesh) {
        DestroyProgram(renderState->gProgramMesh);
    }

    if (renderState->gProgramPost) {
        DestroyProgram(renderState->gProgramPost);
    }

    if (renderState->gVAO_Mesh) {
        glDeleteVertexArrays(1, &renderState->gVAO_Mesh);
    }

    if (renderState->gVBO_Mesh) {
        glDeleteBuffers(1, &renderState->gVBO_Mesh);
    }

    if (renderState->gEBO_Mesh) {
        glDeleteBuffers(1, &renderState->gEBO_Mesh);
    }

    if (renderState->gVAO_Post) {
        glDeleteVertexArrays(1, &renderState->gVAO_Post);
    }

    if (renderState->gVBO_Post) {
        glDeleteBuffers(1, &renderState->gVBO_Post);
    }

    if (gTex_Albedo) {
        DestroyTexture(gTex_Albedo);
    }

    DestroyUBOs();

    ShutdownAudio();
    wglMakeCurrent(nullptr, nullptr);
    if (g_gl.rc)
        wglDeleteContext(g_gl.rc);
    if (g_win.hdc)
        ReleaseDC(g_win.hwnd, g_win.hdc);
    if (g_win.hwnd)
        DestroyWindow(g_win.hwnd);

    UnregisterClassA("RastralEngine", g_win.hinst);
    return 0;
}
