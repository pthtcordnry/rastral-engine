#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <GL/glew.h>
#include <GL/wglew.h>

#include "matrix_helper.h"
#include "renderer.cpp"
#include "opengl_renderer.cpp"
#include "MusicDirector.cpp"
#include "windows_input.cpp"

// ---------------- Window / GL boilerplate ----------------
struct Win32Window { HINSTANCE hinst; HWND hwnd; HDC hdc; int width; int height; bool running; };
struct GLContext { HGLRC rc; bool core; };
struct HiResTimer { LARGE_INTEGER f; LARGE_INTEGER t0; };

Win32Window g_win = {};
GLContext   g_gl = {};
HiResTimer  g_tim = {};

audio_engine  g_audio = {};
MusicDirector g_md = {};
bool          g_audioReady = false;
float         g_rage = 0.0f;
bool          g_vsyncOn = true;

std::string gMeshShaderBase = "shaders/simple_uv";
std::string gPostShaderBase = "shaders/visualizer";
std::string gTexturePath = "textures/melty.png";

RenderTarget gRT_Scene = {};
GLuint gProgramMesh = 0;
GLuint gProgramPost = 0;

GLuint gVAO_Mesh = 0, gVBO_Mesh = 0, gEBO_Mesh = 0;
GLuint gVAO_Post = 0, gVBO_Post = 0;

GLuint gTex_Albedo = 0;

float NowSecs(const HiResTimer& t) { 
    LARGE_INTEGER n; 
    QueryPerformanceCounter(&n); 
    return float(double(n.QuadPart - t.t0.QuadPart) / double(t.f.QuadPart)); 
}

void  TimerStart(HiResTimer& t) { 
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
            } else { 
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

bool InitAudio() {
    audio_config engCfg = { 0,0,18000.0,4 };
    if (!ae_init(&g_audio, &engCfg)) {
        return false;
    }

    MD_Settings s{}; 
    s.bpm = 110.f; 
    s.initialStartDelaySec = 0.15f; 
    md_init(&g_md, &g_audio, &s);
    std::vector<MD_StemDesc> stems = {
        {"drums","audio/drums.flac",true,MAE_ThroughLPF},
        {"bass","audio/bass.flac",false,MAE_ThroughLPF},
        {"percussion","audio/percussion.flac",false,MAE_ThroughLPF},
        {"synth","audio/synth.flac",false,MAE_ThroughLPF},
        {"lead","audio/synth_lead.flac",false,MAE_ThroughLPF},
    };

    if (!md_load_stems(&g_md, stems)) {
        return false;
    }

    if (!md_start_all_synced_looping(&g_md)) {
        return false;
    }

    md_set_state(&g_md, MD_Calm, false, 0.0f);
    g_audioReady = true;
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
    snprintf(title, sizeof(title), "Rastral Engine | state=%s rage=%.2f vsync=%s", StateName(md_get_state(&g_md)), g_rage, g_vsyncOn ? "on" : "off");
    SetWindowTextA(g_win.hwnd, title);
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

GLuint LoadProgramFromFiles(const std::string& vsPath, const std::string& fsPath) {
    std::string vsSrc = ReadTextFile(vsPath), fsSrc = ReadTextFile(fsPath);
    if (vsSrc.empty() || fsSrc.empty()) {
        std::string msg = "Missing shader: " + vsPath + " or " + fsPath;
        MessageBoxA(nullptr, msg.c_str(), "Shader Load Error", MB_ICONERROR);
        ExitProcess(1);
    }
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc.c_str());
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
    GLuint p = LinkProgram(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

void LoadShaders() {
    if (gProgramMesh) { 
        glDeleteProgram(gProgramMesh); 
        gProgramMesh = 0; 
    }

    if (gProgramPost) { 
        glDeleteProgram(gProgramPost); 
        gProgramPost = 0; 
    }

    gProgramMesh = LoadProgramFromFiles(gMeshShaderBase + ".vert", gMeshShaderBase + ".frag");
    gProgramPost = LoadProgramFromFiles(gPostShaderBase + ".vert", gPostShaderBase + ".frag");

    glUseProgram(gProgramMesh);
    glUniform1i(glGetUniformLocation(gProgramMesh, "uTex"), 0);
    BindUBOsForMesh(gProgramMesh);
    glUseProgram(0);

    glUseProgram(gProgramPost);
    glUniform1i(glGetUniformLocation(gProgramPost, "uScene"), 0);
    BindUBOsForVisualizer(gProgramPost);
    glUseProgram(0);

    // Create UBOs once
    CreateUBOs();
    BindUBOsForMesh(gProgramMesh);
    BindUBOsForVisualizer(gProgramPost);
    UpdateWindowTitle();
}

void CreateMeshCube() {
    const float verts[] = {
        -1,-1, 1,  0,0,   1,-1, 1,  1,0,   1, 1, 1,  1,1,  -1, 1, 1,  0,1,
        -1,-1,-1, 1,0,  -1, 1,-1, 1,1,   1, 1,-1, 0,1,   1,-1,-1, 0,0,
        -1,-1,-1, 0,0,  -1,-1, 1, 1,0,  -1, 1, 1, 1,1,  -1, 1,-1, 0,1,
         1,-1,-1, 1,0,   1, 1,-1, 1,1,   1, 1, 1, 0,1,   1,-1, 1, 0,0,
         -1, 1, 1, 0,1,   1, 1, 1, 1,1,   1, 1,-1, 1,0,  -1, 1,-1, 0,0,
         -1,-1, 1, 0,0,  -1,-1,-1, 0,1,   1,-1,-1, 1,1,   1,-1, 1, 1,0
    };
    const unsigned short idx[] = {
        0,1,2, 2,3,0,      4,5,6, 6,7,4,
        8,9,10, 10,11,8,   12,13,14, 14,15,12,
        16,17,18, 18,19,16, 20,21,22, 22,23,20
    };

    glGenVertexArrays(1, &gVAO_Mesh);
    glBindVertexArray(gVAO_Mesh);

    glGenBuffers(1, &gVBO_Mesh);
    glBindBuffer(GL_ARRAY_BUFFER, gVBO_Mesh);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenBuffers(1, &gEBO_Mesh);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gEBO_Mesh);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void CreateFullscreenQuad() {
    const float fsq[] = {
        -1.f,-1.f, 0.f,0.f,   1.f,-1.f, 1.f,0.f,   1.f, 1.f, 1.f,1.f,
        -1.f,-1.f, 0.f,0.f,   1.f, 1.f, 1.f,1.f,  -1.f, 1.f, 0.f,1.f
    };
    glGenVertexArrays(1, &gVAO_Post);
    glBindVertexArray(gVAO_Post);

    glGenBuffers(1, &gVBO_Post);
    glBindBuffer(GL_ARRAY_BUFFER, gVBO_Post);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsq), fsq, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void InitGraphics(int width, int height) {
    SetViewportSize(width, height);

    LoadShaders();
    CreateMeshCube();
    CreateFullscreenQuad();
    CreateRenderTarget(gRT_Scene, g_view_w, g_view_h);

    gTex_Albedo = LoadTextureRGBA8_FromFile(gTexturePath.c_str());
    if (gTex_Albedo == 0) {
        MessageBoxA(nullptr, "Texture load failed (textures/melty.png).", "Texture", MB_ICONWARNING);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
}

void RenderFrame(float tSeconds, int viewW, int viewH) {
    SetViewportSize(viewW, viewH);

    BeginRenderTarget(gRT_Scene);
     BeginFrame(0.05f, 0.06f, 0.08f, 1.0f);

      float aspect = (float)g_view_w / (float)g_view_h;
      Mat4 P = matPerspective(60.0f * 3.1415926f / 180.0f, aspect, 0.1f, 100.0f);
      Mat4 V = matIdentity();                 // or your camera look-at if you have one
      Mat4 PV = matMul(P, V);
      Mat4 T = Mat4Translate(0.0f, 0.0f, -3.0f);
      Mat4 R = matRotateY(tSeconds * 0.7f);
      Mat4 M = matMul(T, R);

      // Push to UBOs
      UpdatePerFrameUBO(PV.m);

      const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      UpdatePerDrawUBO(M.m, tint);

      BeginShader(gProgramMesh);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, gTex_Albedo);

      glBindVertexArray(gVAO_Mesh);
      glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
      glBindVertexArray(0);

      glBindTexture(GL_TEXTURE_2D, 0);
     EndShader();
    EndRenderTarget();

    float beatPhase = 0.0f, barPhase = 0.0f; md_music_clock(&g_md, &beatPhase, &barPhase);
    float vDrums = md_get_stem_current_volume(&g_md, "drums");
    float vBass = md_get_stem_current_volume(&g_md, "bass");
    float vPerc = md_get_stem_current_volume(&g_md, "percussion");
    float vSynth = md_get_stem_current_volume(&g_md, "synth");
    float vLead = md_get_stem_current_volume(&g_md, "lead");
    int   stateI = (int)md_get_state(&g_md);

    BeginFrame(0, 0, 0, 1);
     BeginShader(gProgramPost);
      UpdateVizParamsUBO((float)g_view_w, (float)g_view_h, tSeconds, beatPhase, barPhase, stateI, g_rage, vDrums, vBass, vPerc, vSynth, vLead);
     
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, gRT_Scene.color);
     
      glBindVertexArray(gVAO_Post);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glBindVertexArray(0);
     
      glBindTexture(GL_TEXTURE_2D, 0);
     EndShader();
    EndFrame();
}

void HandleInput() {
    const float step = 0.05f;

    if (Input_IsPressed('1')) {
        md_set_state(&g_md, MD_Calm, true, 300.0f);
    }

    if (Input_IsPressed('2')) {
        md_set_state(&g_md, MD_Tense, true, 300.0f);
    }

    if (Input_IsPressed('3')) {
        md_set_state(&g_md, MD_Combat, true, 350.0f);
    }

    if (Input_IsPressed('4')) {
        md_set_state(&g_md, MD_Overdrive, true, 400.0f);
    }

    if (Input_IsPressed(VK_OEM_MINUS)) {
        g_rage = (g_rage - step < 0.0f) ? 0.0f : (g_rage - step);
    }

    if (Input_IsPressed(VK_SUBTRACT)) {
        g_rage = (g_rage - step < 0.0f) ? 0.0f : (g_rage - step);
    }

    if (Input_IsPressed(VK_OEM_PLUS)) {
        g_rage = (g_rage + step > 1.0f) ? 1.0f : (g_rage + step);
    }

    if (Input_IsPressed(VK_ADD)) {
        g_rage = (g_rage + step > 1.0f) ? 1.0f : (g_rage + step);
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
        if (g_gl.rc) SetViewportSize(g_win.width, g_win.height);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
        Input_HandleKeyMsg(m, w, l);
        return 0; // we handled it
    }

    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        // Handle your input, but still allow system behaviors (Alt+F4, Alt+Space, etc.)
        Input_HandleKeyMsg(m, w, l);
        return DefWindowProc(h, m, w, l);
    }

    case WM_ACTIVATEAPP: {
        if (w == FALSE) Input_ClearAll();
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

    // Anything we didn't explicitly handle:
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
    g_vsyncOn = true;

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

        if (g_audioReady) {
            md_set_rage(&g_md, g_rage, true, 180.0f);
            md_update(&g_md, dt);
        }

        RenderFrame(now, g_win.width, g_win.height);
        SwapBuffers(g_win.hdc);
        if (!g_vsyncOn) {
            using namespace std::chrono;
            static auto last = high_resolution_clock::now();
            const auto target = milliseconds(16); 
            auto now = high_resolution_clock::now();
            auto elapsed = now - last;
            if (elapsed < target) std::this_thread::sleep_for(target - elapsed);
            last = now;
        }
    }

    if (gProgramMesh) { 
        glDeleteProgram(gProgramMesh);      
    }
    if (gProgramPost) { 
        glDeleteProgram(gProgramPost);       
    }
    if (gVAO_Mesh) { 
        glDeleteVertexArrays(1, &gVAO_Mesh); 
    }
    if (gVBO_Mesh) { 
        glDeleteBuffers(1, &gVBO_Mesh);      
    }
    if (gEBO_Mesh) { 
        glDeleteBuffers(1, &gEBO_Mesh);      
    }
    if (gVAO_Post) { 
        glDeleteVertexArrays(1, &gVAO_Post); 
    }
    if (gVBO_Post) { 
        glDeleteBuffers(1, &gVBO_Post);     
    }
    if (gTex_Albedo) { 
        glDeleteTextures(1, &gTex_Albedo);   
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
