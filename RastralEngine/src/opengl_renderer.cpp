#include <cstdio>
#include <vector>
#include <GL/glew.h>

static int g_view_w = 1280;
static int g_view_h = 720;

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