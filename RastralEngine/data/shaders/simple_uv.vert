#version 330 core

layout(std140) uniform PerFrame {
    mat4 uProjView;
};
layout(std140) uniform PerDraw {
    mat4 uModel;
    vec4 uTint;
};
layout(std140) uniform Skin {
    mat4 uBones[128];
    int  uBoneCount; // unused by shader, but available
    ivec3 _pad;
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aJoints;   // as floats; will round to ints
layout(location = 3) in vec4 aWeights;  // weights

out vec2 vUV;
out vec4 vTint;

void main() {
    // Normalize weights defensively
    vec4 w = aWeights;
    float s = w.x + w.y + w.z + w.w;
    if (s > 0.00001) w /= s; else w = vec4(1.0, 0.0, 0.0, 0.0);

    ivec4 j = ivec4(round(aJoints));

    // Fetch with bounds safety
    mat4 B0 = (j.x >= 0 && j.x < 128) ? uBones[j.x] : mat4(1.0);
    mat4 B1 = (j.y >= 0 && j.y < 128) ? uBones[j.y] : mat4(1.0);
    mat4 B2 = (j.z >= 0 && j.z < 128) ? uBones[j.z] : mat4(1.0);
    mat4 B3 = (j.w >= 0 && j.w < 128) ? uBones[j.w] : mat4(1.0);

    vec4 p = vec4(aPos, 1.0);
    vec4 skinned = (B0 * p) * w.x +
                   (B1 * p) * w.y +
                   (B2 * p) * w.z +
                   (B3 * p) * w.w;

    vUV   = aUV;
    vTint = uTint;
    gl_Position = uProjView * uModel * skinned;
}
