#include <cstdio>
#include <vector>
#include <GL/glew.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int g_view_w = 1280;
static int g_view_h = 720;

struct RenderTarget {
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
};

bool CreateRenderTarget(RenderTarget& rt, int w, int h) {
    rt.w = w; rt.h = h;

    glGenTextures(1, &rt.color);
    glBindTexture(GL_TEXTURE_2D, rt.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &rt.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rt.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.depth);

    bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void DestroyRenderTarget(RenderTarget& rt) {
    if (rt.depth) { glDeleteRenderbuffers(1, &rt.depth); rt.depth = 0; }
    if (rt.color) { glDeleteTextures(1, &rt.color); rt.color = 0; }
    if (rt.fbo) { glDeleteFramebuffers(1, &rt.fbo); rt.fbo = 0; }
    rt.w = rt.h = 0;
}

void BeginRenderTarget(RenderTarget& rt) {
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glViewport(0, 0, rt.w, rt.h);
}

void EndRenderTarget() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_view_w, g_view_h); // your globals
}

void SetViewportSize(int width, int height) {
    g_view_w = (width > 0) ? width : 1;
    g_view_h = (height > 0) ? height : 1;
    glViewport(0, 0, g_view_w, g_view_h);
}

void BeginFrame(float r, float g, float b, float a) {
    glViewport(0, 0, g_view_w, g_view_h);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void EndFrame() {
    glFlush();
}

void BeginShader(GLuint program) { 
    glUseProgram(program); 
}

void EndShader() { 
    glUseProgram(0); 
}

void Set1f(GLuint program, const char* name, float x) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1f(loc, x);
}
void Set2f(GLuint program, const char* name, float x, float y) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform2f(loc, x, y);
}

void Set1i(GLuint program, const char* name, int x) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, x);
}
void Set3f(GLuint program, const char* name, float x, float y, float z) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform3f(loc, x, y, z);
}
void Set4f(GLuint program, const char* name, float x, float y, float z, float w) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}

void BindVAO(GLuint vao) { 
    glBindVertexArray(vao); 
}

void DrawTriangles(GLint first, GLsizei count) { 
    glDrawArrays(GL_TRIANGLES, first, count); 
}

GLuint CompileShader(GLenum type, const char* source_utf8) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source_utf8, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetShaderInfoLog(s, len, nullptr, log.data());
        std::fprintf(stderr, "[shader] compile error:\n%s\n", log.data());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint LinkProgram(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetProgramInfoLog(p, len, nullptr, log.data());
        std::fprintf(stderr, "[link] error:\n%s\n", log.data());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint CreateTexture2D(int width, int height,
    GLenum internalFormat, GLenum srcFormat,
    GLenum srcType, const void* pixels,
    GLint minFilter = GL_LINEAR, GLint magFilter = GL_LINEAR,
    GLint wrapS = GL_CLAMP_TO_EDGE, GLint wrapT = GL_CLAMP_TO_EDGE)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Safe for 3-channel uploads too
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
        width, height, 0, srcFormat, srcType, pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);

    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // restore default
    return tex;
}

void UpdateTexture2D(GLuint tex, int width, int height,
    GLenum srcFormat, GLenum srcType, const void* pixels)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, srcFormat, srcType, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void DestroyTexture(GLuint tex) {
    if (tex) glDeleteTextures(1, &tex);
}

void BindTexture2D(GLuint unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

GLuint LoadTextureRGBA8_FromFile(const char* path, bool flipY = true)
{
    if (flipY) stbi_set_flip_vertically_on_load(1);
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load(path, &w, &h, &n, 4); // force RGBA
    if (!rgba) { std::fprintf(stderr, "stb_image: failed to load %s\n", path); return 0; }

    GLuint tex = CreateTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgba,
        GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    stbi_image_free(rgba);
    return tex;
}