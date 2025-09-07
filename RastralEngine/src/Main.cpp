#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <GL/glew.h>
#include <GL/wglew.h>
#include "opengl_renderer.cpp"
#include "MusicDirector.cpp"

struct Win32Window {
    HINSTANCE hinst{};
    HWND      hwnd{};
    HDC       hdc{};
    int       width = 1280;
    int       height = 720;
    bool      running = true;
};

struct GLContext {
    HGLRC rc{};
    bool  core = false;
};

struct HiResTimer { 
    LARGE_INTEGER f{}, t0{}; 
};

Win32Window g_win;
GLContext   g_gl;

void  TimerStart(HiResTimer& t) { QueryPerformanceFrequency(&t.f); QueryPerformanceCounter(&t.t0); }
float NowSecs(const HiResTimer& t) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return float(double(n.QuadPart - t.t0.QuadPart) / double(t.f.QuadPart)); }

audio_engine g_audio{};
MusicDirector g_md{};
bool g_audioReady = false;
float g_rage = 0.0f;

std::string g_vsPath = "shaders/visualizer.vert";
std::string g_fsPath = "shaders/visualizer.frag";

GLuint gVAO = 0, gVBO = 0, gProg = 0;

void Win32Fail(const char* where) {
    DWORD e = GetLastError();
    char msg[1024]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, msg, sizeof(msg), nullptr);
    MessageBoxA(nullptr, msg, where, MB_ICONERROR);
}

void UpdateWindowTitle() {
    if (!g_win.hwnd) return;
    char title[256];
    sprintf_s(title, "%s | state=%s rage=%.2f",
        "Rastral Engine", StateName(md_get_state(&g_md)), g_rage);
    SetWindowTextA(g_win.hwnd, title);
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool InitAudio() {
    audio_config engCfg = { 0, 0, 18000.0, 4 };
    if (!ae_init(&g_audio, &engCfg)) {
        return false;
    }

    MD_Settings cfg;
    cfg.bpm = 110.f;
    cfg.initialStartDelaySec = 0.15f;
    md_init(&g_md, &g_audio, &cfg);

    std::vector<MD_StemDesc> stems = {
        {"drums",  "audio/drums.flac", true,  MAE_ThroughLPF},
        {"bass",  "audio/bass.flac",   false, MAE_ThroughLPF},
        {"percussion", "audio/percussion.flac", false, MAE_ThroughLPF},
        {"synth",  "audio/synth.flac", false, MAE_ThroughLPF},
        {"lead",  "audio/synth_lead.flac", false, MAE_ThroughLPF},
    };

    if (!md_load_stems(&g_md, stems)) {
        return false;
    }
    if (!md_start_all_synced_looping(&g_md)) {
        return false;
    }

    md_set_state(&g_md, MD_Calm, false, 0.0f);
    g_audioReady = true;
    UpdateWindowTitle();
    return true;
}

void ShutdownAudio() {
    if (!g_audioReady) {
        return;
    }
    md_shutdown(&g_md);
    ae_shutdown(&g_audio);
    g_audioReady = false;
}

void LoadShaders() {
    std::string vsSrc = ReadTextFile(g_vsPath);
    std::string fsSrc = ReadTextFile(g_fsPath);

    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc.c_str());
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
    gProg = LinkProgram(vs, fs);
    glDeleteShader(vs); 
    glDeleteShader(fs);
    if (!gProg) { 
        MessageBoxA(nullptr, "Could not compile fallback shaders", "Fatal", MB_ICONERROR); 
        ExitProcess(1); 
    }
}

void OnKeyDown(WPARAM vk) {
    const float step = 0.05f;
    switch (vk)
    {
        case '1': {
            md_set_state(&g_md, MD_Calm, true, 300.0f);
        } break;
        
        case '2':{
            md_set_state(&g_md, MD_Tense, true, 300.0f);
        } break;
        
        case '3': {
            md_set_state(&g_md, MD_Combat, true, 350.0f);
        } break;
        
        case '4': {
            md_set_state(&g_md, MD_Overdrive, true, 400.0f);
        } break;
        
        case VK_OEM_MINUS:
        case VK_SUBTRACT: {
            g_rage -= step;
            if (g_rage < 0.0f) {
                g_rage = 0.0f;
            }
        } break;

        case VK_OEM_PLUS:
        case VK_ADD: {
            g_rage += step;
            if (g_rage > 1.0f) {
                g_rage = 1.0f;
            }
        } break;
        case 'R': {
            LoadShaders();
        } break;

        case 'Q':
        case VK_ESCAPE: {
            PostMessageA(g_win.hwnd, WM_CLOSE, 0, 0);
        } break;
    }

    UpdateWindowTitle();
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE: {
            g_win.width = int(LOWORD(l));
            g_win.height = int(HIWORD(l));
            if (g_gl.rc) SetViewportSize(g_win.width, g_win.height);
            return 0;
        } break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (g_audioReady) OnKeyDown(w);
            return 0;
        } break;

        case WM_CLOSE: {
            DestroyWindow(h);
            return 0;
        } break;

        case WM_DESTROY: {
            g_win.running = false;
            PostQuitMessage(0);
            return 0;
        } break;

        default: {
            return DefWindowProc(h, m, w, l);
        } break;
    }
}

bool Win32CreateWindowSimple(Win32Window& w) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = w.hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RastralEngine";

    if (!RegisterClassExA(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) { Win32Fail("RegisterClassExA"); return false; }
    }

    RECT r{ 0,0,w.width,w.height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    w.hwnd = CreateWindowExA(
        0, wc.lpszClassName, "Rastral Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, w.hinst, nullptr);
    if (!w.hwnd) { Win32Fail("CreateWindowExA"); return false; }

    w.hdc = GetDC(w.hwnd);
    if (!w.hdc) { Win32Fail("GetDC"); return false; }

    ShowWindow(w.hwnd, SW_SHOW);
    UpdateWindow(w.hwnd);
    return true;
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
    if (!fmt) { Win32Fail("ChoosePixelFormat"); return false; }
    if (!SetPixelFormat(hdc, fmt, &pfd)) { Win32Fail("SetPixelFormat"); return false; }
    return true;
}

bool GLCreateContextPreferCore(HDC hdc, GLContext& c) {
    HGLRC temp = wglCreateContext(hdc);
    if (!temp) { Win32Fail("wglCreateContext(temp)"); return false; }
    if (!wglMakeCurrent(hdc, temp)) { Win32Fail("wglMakeCurrent(temp)"); return false; }

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        c.rc = temp;
        c.core = false;
        return true;
    }

    if (wglCreateContextAttribsARB) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
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
                if (glewInit() != GLEW_OK) { Win32Fail("glewInit(core)"); return false; }
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
    MSG m;
    while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) w.running = false;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return w.running;
}

void Init(int width, int height) {
    SetViewportSize(width, height);

    glGenVertexArrays(1, &gVAO);
    glBindVertexArray(gVAO);
    const float verts[] = { -0.8f,-0.6f,  0.8f,-0.6f,  0.0f,0.8f };
    glGenBuffers(1, &gVBO);
    glBindBuffer(GL_ARRAY_BUFFER, gVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    LoadShaders();
}

void Render(float t, int w, int h) {
    SetViewportSize(w, h);
    BeginFrame(0.05f, 0.06f, 0.08f, 1.0f);

    float beatPhase = 0.f, barPhase = 0.f;
    md_music_clock(&g_md, &beatPhase, &barPhase);
    const float vDrums = md_get_stem_current_volume(&g_md, "drums");
    const float vBass = md_get_stem_current_volume(&g_md, "bass");
    const float vPerc = md_get_stem_current_volume(&g_md, "percussion");
    const float vSynth = md_get_stem_current_volume(&g_md, "synth");
    const float vLead = md_get_stem_current_volume(&g_md, "lead");
    const int   stateI = (int)md_get_state(&g_md);
    const float rage = g_rage;

    BeginShader(gProg);
    Set1f(gProg, "uTime", t);
    Set2f(gProg, "uRes", (float)w, (float)h);

    Set1f(gProg, "uBeatPhase", beatPhase);
    Set1f(gProg, "uBarPhase", barPhase);
    Set1i(gProg, "uState", stateI);
    Set1f(gProg, "uRage", rage);
    Set4f(gProg, "uLevelsA", vDrums, vBass, vPerc, vSynth);
    Set1f(gProg, "uLevelLead", vLead);

    BindVAO(gVAO);
    DrawTriangles(0, 3);
    BindVAO(0);

    EndShader();
    EndFrame();
}

extern "C" int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_win.hinst = hInst;

    if (!Win32CreateWindowSimple(g_win))                  return 1;
    if (!Win32SetBasicPixelFormat(g_win.hdc))             return 1;
    if (!GLCreateContextPreferCore(g_win.hdc, g_gl))      return 1;

    if (!InitAudio()) { MessageBoxA(nullptr, "Audio init failed.", "Error", MB_ICONERROR); /*continue headless or*/ return 2; }

    Init(g_win.width, g_win.height);
    UpdateWindowTitle();

    HiResTimer timer; TimerStart(timer);
    float lastT = NowSecs(timer);

    // Main loop
    while (PumpMessages(g_win)) {
        float t = NowSecs(timer);
        float dt = t - lastT;
        lastT = t;

        if (g_audioReady) {
            md_set_rage(&g_md, g_rage, true, 180.f);
            md_update(&g_md, dt);
        }

        Render(t, g_win.width, g_win.height);
        SwapBuffers(g_win.hdc);

        Sleep(1);
    }

    ShutdownAudio();

    wglMakeCurrent(nullptr, nullptr);
    if (g_gl.rc) wglDeleteContext(g_gl.rc);
    if (g_win.hdc) ReleaseDC(g_win.hwnd, g_win.hdc);
    if (g_win.hwnd) DestroyWindow(g_win.hwnd);
    UnregisterClassA("RastralEngine", g_win.hinst);
    return 0;
}
