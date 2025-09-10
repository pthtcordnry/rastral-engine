#version 330 core

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uScene;

layout(std140) uniform VizParams {
    vec2  uRes;         // pixels: (width, height)
    float uTime;
    float _pad0;

    float uBeatPhase;
    float uBarPhase;
    int   uState;
    float uRage;

    vec4  uLevelsA;     // x=drums, y=bass, z=perc, w=synth
    float uLevelLead;
    float _pad2[3];
};

vec3 stateColor(int s){
    if (s==0) return vec3(0.10,0.15,0.25);
    if (s==1) return vec3(0.16,0.18,0.28);
    if (s==2) return vec3(0.22,0.14,0.10);
    return      vec3(0.25,0.10,0.10);
}

void main() {
    // 1) base scene
    vec4 scene = texture(uScene, vUV);

    // 2) legacy geometry / envelope
    vec2 uv = vUV * 2.0 - 1.0;
    uv.x *= (uRes.x / uRes.y);

    float drums = uLevelsA.x;
    float bass  = uLevelsA.y;
    float perc  = uLevelsA.z;
    float synth = uLevelsA.w;
    float lead  = uLevelLead;

    // legacy pulse shaping (kept for original feel)
    float beatPulse = smoothstep(0.0, 1.0, sin(6.28318*uBeatPhase)*0.5 + 0.5);
    beatPulse = pow(beatPulse, 2.0);

    vec3 base = stateColor(uState);
    base += vec3(0.25, 0.05, 0.00) * uRage; // warm rage

    float d = length(uv);
    float radius = mix(0.45, 0.25, bass);
    float thick  = mix(0.015, 0.06, drums * beatPulse);

    float ring    = smoothstep(radius - thick, radius, d) - smoothstep(radius, radius + thick, d);
    float dash    = 0.5 + 0.5 * sin(30.0 * atan(uv.y, uv.x) + 10.0 * uTime);
    float rim     = smoothstep(0.95, 0.97, 1.0 - d) * perc * dash;
    float shimmer = 0.1 * synth * (0.5 + 0.5 * sin(8.0 * uTime + 20.0 * d));
    float glow    = 0.4 * lead  * exp(-10.0 * abs(d - radius));

    // 4) composite (legacy balance + ripple contribution)
    vec3 overlay = base * 0.2 + 0.9 * ring + 0.6 * rim + (shimmer + glow);
    vec3 outRGB  = clamp(scene.rgb + overlay, 0.0, 1.0);

    FragColor = vec4(outRGB, 1.0);
}
