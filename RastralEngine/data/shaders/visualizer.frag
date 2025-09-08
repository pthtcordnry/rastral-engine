#version 330 core
in vec2 VUV;
out vec4 FragColor;

uniform sampler2D uScene;   // <-- scene color from pass 1

// Music uniforms (optional; keep them if you’re already setting them)
uniform float uTime;
uniform vec2  uRes;
uniform float uBeatPhase;
uniform float uBarPhase;
uniform int   uState;     // MD_Calm..MD_Overdrive
uniform float uRage;      // 0..1
uniform vec4  uLevelsA;   // x=drums, y=bass, z=perc, w=synth
uniform float uLevelLead; // lead level

vec3 stateColor(int s){
    if (s==0) return vec3(0.10,0.15,0.25);
    if (s==1) return vec3(0.16,0.18,0.28);
    if (s==2) return vec3(0.22,0.14,0.10);
    return      vec3(0.25,0.10,0.10);
}

void main() {
    // 1) Always start from the scene
    vec4 scene = texture(uScene, VUV);

    // 2) Compute visualizer overlay (ring etc.)
    // Convert VUV into -1..1 and fix aspect
    vec2 uv = VUV * 2.0 - 1.0;
    uv.x *= (uRes.x / uRes.y);

    float beatPulse = smoothstep(0.0, 1.0, sin(6.28318*uBeatPhase)*0.5+0.5);
    beatPulse = pow(beatPulse, 2.0);

    float drums = uLevelsA.x;
    float bass  = uLevelsA.y;
    float perc  = uLevelsA.z;
    float synth = uLevelsA.w;
    float lead  = uLevelLead;

    vec3 base = stateColor(uState);
    base += vec3(0.25,0.05,0.00) * uRage;

    float d = length(uv);
    float radius = mix(0.45, 0.25, bass);
    float thick  = mix(0.015, 0.06, drums*beatPulse);

    float ring = smoothstep(radius - thick, radius, d) - smoothstep(radius, radius + thick, d);
    float dash = 0.5 + 0.5*sin(30.0*atan(uv.y, uv.x) + 10.0*uTime);
    float rim  = smoothstep(0.95, 0.97, 1.0 - d) * perc * dash;
    float shimmer = 0.1 * synth * (0.5 + 0.5*sin(8.0*uTime + 20.0*d));
    float glow = 0.4 * lead * exp(-10.0*abs(d - radius));

    vec3 overlay = base*0.2 + 0.9*ring + 0.6*rim + (shimmer + glow);

    // 3) Composite: add on top of scene (or mix with an alpha if you prefer)
    vec3 outRGB = clamp(scene.rgb + overlay, 0.0, 1.0);
    FragColor = vec4(outRGB, 1.0);
}
