#version 330 core

in vec2 vUV;
in vec4 vTint;

uniform sampler2D uTex;

out vec4 FragColor;

void main() {
    vec4 tex = texture(uTex, vUV);
    FragColor = tex * vTint;
}
