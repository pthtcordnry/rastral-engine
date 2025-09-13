#version 330 core

in vec2 vUV;
in vec4 vTint;
out vec4 FragColor;

uniform sampler2D uTex;

void main() {
    vec4 albedo = texture(uTex, vUV);
    FragColor = albedo * vTint;
}
