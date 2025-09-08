#version 330 core
in vec2 UV;
out vec4 FragColor;

uniform sampler2D uTex;   // set to texture unit 0 in C++
uniform vec4      uTint;  // set to (1,1,1,1)

void main() {
    FragColor = texture(uTex, UV) * uTint;
}
