#version 330 core
layout(location=0) in vec2 POS;     // fullscreen quad position in NDC (-1..1)
layout(location=1) in vec2 UVIN;    // 0..1
out vec2 VUV;

void main() {
    VUV = UVIN;
    gl_Position = vec4(POS, 0.0, 1.0);
}
