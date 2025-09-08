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
    HINSTANCE hinst;
    HWND      hwnd;
    HDC       hdc;
    int       width;
    int       height;
    bool      running;
};
struct GLContext {
    HGLRC rc;
    bool  core;
};
struct HiResTimer {
    LARGE_INTEGER f;
    LARGE_INTEGER t0;
};

Win32Window  g_win = {};
GLContext    g_gl = {};
HiResTimer   g_timer = {};

audio_engine  g_audio = {};
MusicDirector g_md = {};
bool          g_audioOk = false;
float         g_rage = 0.0f;

std::string g_visualShaderBase = "shaders/visualizer";
std::string g_uvShaderBase = "shaders/simple_uv";
std::string g_texturePath = "textures/melty.png";

RenderTarget gScene = {};
GLuint sceneVAO = 0, sceneVBO = 0;
GLuint postVAO = 0, postVBO = 0;
GLuint progUV = 0, progPost = 0;
GLuint texCheckerOrFile = 0;

// -------------------- Utils --------------------
void Win32Fail(const char* where) {
    DWORD err = GetLastError();
    char  msg[1024] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, msg, sizeof(msg), nullptr);
    MessageBoxA(nullptr, msg, where, MB_ICONERROR);
}

void TimerStart(HiResTimer& t) { QueryPerformanceFrequency(&t.f); QueryPerformanceCounter(&t.t0); }
float NowSecs(const HiResTimer& t) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return float(double(n.QuadPart - t.t0.QuadPart) / double(t.f.QuadPart)); }

std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void UpdateWindowTitle() {
    if (!g_win.hwnd) return;
    char title[256];
    std::snprintf(title, sizeof(title), "Rastral Engine | state=%s rage=%.2f", StateName(md_get_state(&g_md)), g_rage);
    SetWindowTextA(g_win.hwnd, title);
}

void SetSwapInterval(int interval) {
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(interval);
    }
}

bool InitAudio() {
    audio_config engCfg = { 0, 0, 18000.0, 4 };
    if (!ae_init(&g_audio, &engCfg)) return false;

    MD_Settings cfg = {};
    cfg.bpm = 110.f;
    cfg.initialStartDelaySec = 0.15f;
    md_init(&g_md, &g_audio, &cfg);

    std::vector<MD_StemDesc> stems = {
        {"drums",  "audio/drums.flac", true,  MAE_ThroughLPF},
        {"bass",   "audio/bass.flac",  false, MAE_ThroughLPF},
        {"percussion", "audio/percussion.flac", false, MAE_ThroughLPF},
        {"synth",  "audio/synth.flac", false, MAE_ThroughLPF},
        {"lead",   "audio/synth_lead.flac", false, MAE_ThroughLPF},
    };
    if (!md_load_stems(&g_md, stems)) return false;
    if (!md_start_all_synced_looping(&g_md)) return false;

    md_set_state(&g_md, MD_Calm, false, 0.0f);
    g_audioOk = true;
    UpdateWindowTitle();
    return true;
}

void ShutdownAudio() {
    if (!g_audioOk) return;
    md_shutdown(&g_md);
    ae_shutdown(&g_audio);
    g_audioOk = false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_SIZE:
        g_win.width = int(LOWORD(lparam));
        g_win.height = int(HIWORD(lparam));
        if (g_gl.rc) SetViewportSize(g_win.width, g_win.height);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const float step = 0.05f;
        switch (wparam) {
        case '1': md_set_state(&g_md, MD_Calm, true, 300.0f); break;
        case '2': md_set_state(&g_md, MD_Tense, true, 300.0f); break;
        case '3': md_set_state(&g_md, MD_Combat, true, 350.0f); break;
        case '4': md_set_state(&g_md, MD_Overdrive, true, 400.0f); break;

        case VK_OEM_MINUS:
        case VK_SUBTRACT: g_rage = (g_rage - step < 0.0f) ? 0.0f : (g_rage - step); break;
        case VK_OEM_PLUS:
        case VK_ADD:      g_rage = (g_rage + step > 1.0f) ? 1.0f : (g_rage + step); break;

        case 'V': // toggle vsync
            SetSwapInterval(1);
            break;

        case 'Q':
        case VK_ESCAPE:
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        UpdateWindowTitle();
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_win.running = false;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

bool Win32CreateWindowSimple(Win32Window& w) {
    WNDCLASSEXA wc = {};
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

    RECT r = { 0,0,w.width,w.height };
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
    PIXELFORMATDESCRIPTOR pfd = {};
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
    if (!SetPixelFormat(hdc, fmt, &pfd)) { Win32Fail("SetPixelFormat");    return false; }
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
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) w.running = false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return w.running;
}

// -------------------- Shaders --------------------
void LoadShaders() {
    // simple_uv
    {
        std::string vsPath = g_uvShaderBase + ".vert";
        std::string fsPath = g_uvShaderBase + ".frag";
        std::string vsSrc = ReadTextFile(vsPath);
        std::string fsSrc = ReadTextFile(fsPath);
        if (vsSrc.empty() || fsSrc.empty()) {
            MessageBoxA(nullptr, ("Missing shader: " + vsPath + " or " + fsPath).c_str(), "simple_uv", MB_ICONERROR);
            ExitProcess(1);
        }
        GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc.c_str());
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
        if (progUV) glDeleteProgram(progUV);
        progUV = LinkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        glUseProgram(progUV);
        GLint locTex = glGetUniformLocation(progUV, "uTex");
        if (locTex >= 0) glUniform1i(locTex, 0);
        glUseProgram(0);
    }

    // visualizer/post
    {
        std::string vsPath = g_visualShaderBase + ".vert";
        std::string fsPath = g_visualShaderBase + ".frag";
        std::string vsSrc = ReadTextFile(vsPath);
        std::string fsSrc = ReadTextFile(fsPath);
        if (vsSrc.empty() || fsSrc.empty()) {
            MessageBoxA(nullptr, ("Missing shader: " + vsPath + " or " + fsPath).c_str(), "visualizer", MB_ICONERROR);
            ExitProcess(1);
        }
        GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc.c_str());
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
        if (progPost) glDeleteProgram(progPost);
        progPost = LinkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        glUseProgram(progPost);
        GLint locScene = glGetUniformLocation(progPost, "uScene");
        if (locScene >= 0) glUniform1i(locScene, 0);
        glUseProgram(0);
    }
}

void InitGraphics(int width, int height) {
    SetViewportSize(width, height);

    // Scene triangle (pos.xy, uv.xy)
    const float triVerts[] = {
        -0.8f, -0.6f,   0.0f, 0.0f,
         0.8f, -0.6f,   1.0f, 0.0f,
         0.0f,  0.8f,   0.5f, 1.0f,
    };
    glGenVertexArrays(1, &sceneVAO);
    glBindVertexArray(sceneVAO);
    glGenBuffers(1, &sceneVBO);
    glBindBuffer(GL_ARRAY_BUFFER, sceneVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triVerts), triVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // Post full-screen quad (pos.xy, uv.xy)
    const float fsq[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f, -1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 1.f
    };
    glGenVertexArrays(1, &postVAO);
    glBindVertexArray(postVAO);
    glGenBuffers(1, &postVBO);
    glBindBuffer(GL_ARRAY_BUFFER, postVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsq), fsq, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    LoadShaders();
    CreateRenderTarget(gScene, g_view_w, g_view_h);

    // Try file texture first, fallback to checker
    texCheckerOrFile = LoadTextureRGBA8_FromFile(g_texturePath.c_str());
    if (texCheckerOrFile == 0) {
        MessageBoxA(nullptr, "Texture Init Failed.", "Error", MB_ICONERROR);
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    glUseProgram(progUV);
    GLint tintLoc = glGetUniformLocation(progUV, "uTint");
    if (tintLoc >= 0) glUniform4f(tintLoc, 1.f, 1.f, 1.f, 1.f);
    glUseProgram(0);
}

void RenderFrame(float timeSec, int viewW, int viewH) {
    SetViewportSize(viewW, viewH);

    // Pass 1: draw textured triangle into gScene
    BeginRenderTarget(gScene);
    BeginFrame(0.05f, 0.06f, 0.08f, 1.0f);

    glUseProgram(progUV);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texCheckerOrFile);
    glBindVertexArray(sceneVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    EndRenderTarget();

    // Gather music uniforms
    float beatPhase = 0.0f, barPhase = 0.0f;
    md_music_clock(&g_md, &beatPhase, &barPhase);
    float vDrums = md_get_stem_current_volume(&g_md, "drums");
    float vBass = md_get_stem_current_volume(&g_md, "bass");
    float vPerc = md_get_stem_current_volume(&g_md, "percussion");
    float vSynth = md_get_stem_current_volume(&g_md, "synth");
    float vLead = md_get_stem_current_volume(&g_md, "lead");
    int   stateI = (int)md_get_state(&g_md);

    // Pass 2: post/visualizer to backbuffer
    BeginFrame(0.f, 0.f, 0.f, 1.f);

    glUseProgram(progPost);
    Set1f(progPost, "uTime", timeSec);
    Set2f(progPost, "uRes", (float)g_view_w, (float)g_view_h);
    Set1f(progPost, "uBeatPhase", beatPhase);
    Set1f(progPost, "uBarPhase", barPhase);
    Set1i(progPost, "uState", stateI);
    Set1f(progPost, "uRage", g_rage);
    Set4f(progPost, "uLevelsA", vDrums, vBass, vPerc, vSynth);
    Set1f(progPost, "uLevelLead", vLead);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gScene.color);

    glBindVertexArray(postVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    EndFrame();
}

extern "C" int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_win.hinst = hInst;
    g_win.width = 1280;
    g_win.height = 720;
    g_win.running = true;

    if (!Win32CreateWindowSimple(g_win))             return 1;
    if (!Win32SetBasicPixelFormat(g_win.hdc))        return 1;
    if (!GLCreateContextPreferCore(g_win.hdc, g_gl)) return 1;

    SetSwapInterval(1); // vsync on

    if (!InitAudio()) {
        MessageBoxA(nullptr, "Audio init failed.", "Error", MB_ICONERROR);
        return 2;
    }

    InitGraphics(g_win.width, g_win.height);
    UpdateWindowTitle();

    TimerStart(g_timer);
    float lastTime = NowSecs(g_timer);

    while (PumpMessages(g_win)) {
        float nowTime = NowSecs(g_timer);
        float dt = nowTime - lastTime;
        lastTime = nowTime;

        if (g_audioOk) {
            md_set_rage(&g_md, g_rage, true, 180.f);
            md_update(&g_md, dt);
        }

        RenderFrame(nowTime, g_win.width, g_win.height);
        SwapBuffers(g_win.hdc);
        Sleep(1);
    }

    // Cleanup (best effort; renderer target cleanup depends on your helper)
    if (progUV)   glDeleteProgram(progUV);
    if (progPost) glDeleteProgram(progPost);
    if (sceneVBO) glDeleteBuffers(1, &sceneVBO);
    if (postVBO)  glDeleteBuffers(1, &postVBO);
    if (sceneVAO) glDeleteVertexArrays(1, &sceneVAO);
    if (postVAO)  glDeleteVertexArrays(1, &postVAO);
    if (texCheckerOrFile) glDeleteTextures(1, &texCheckerOrFile);

    ShutdownAudio();

    wglMakeCurrent(nullptr, nullptr);
    if (g_gl.rc) {
        wglDeleteContext(g_gl.rc);
    }

    if (g_win.hdc) {
        ReleaseDC(g_win.hwnd, g_win.hdc);
    }

    if (g_win.hwnd) {
        DestroyWindow(g_win.hwnd);
    }

    UnregisterClassA("RastralEngine", g_win.hinst);
    return 0;
}
