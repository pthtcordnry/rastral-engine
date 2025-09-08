#version 330 core
layout(location=0) in vec2 POS;
layout(location=1) in vec2 UVCOORD;
out vec2 UV;

void main() {
    UV = UVCOORD;
    gl_Position = vec4(POS, 0.0, 1.0);
}
