#version 330 core

layout(std140) uniform PerFrame {
    mat4 uProjView;
};

layout(std140) uniform PerDraw {
    mat4 uModel;
    vec4 uTint;
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

out vec2 vUV;
out vec4 vTint;

void main() {
    vUV   = aUV;
    vTint = uTint;
    gl_Position = uProjView * uModel * vec4(aPos, 1.0);
}
