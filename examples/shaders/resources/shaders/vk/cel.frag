#version 450

// Port of glsl330/cel.fs to the rayvulkan shader ABI

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec3 fragNormal;

#define MAX_LIGHTS 4

// rlights.h compatible light block, flattened to plain arrays (see lighting.frag).
// Declared IDENTICALLY in both stages (single set 0 binding 0).
layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec3 viewPos;               // View position for future specular / fresnel use
    float numBands;             // Number of discrete toon bands (2 = hard binary, 20 = near-smooth)
    mat4 matNormal;
    int lightsEnabled[MAX_LIGHTS];
    int lightsType[MAX_LIGHTS]; // 0 = directional, 1 = point
    vec3 lightsPosition[MAX_LIGHTS];
    vec3 lightsTarget[MAX_LIGHTS];
    vec4 lightsColor[MAX_LIGHTS];
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main() {
    vec4 texColor = texture(texture0, fragTexCoord);
    vec3 baseColor = texColor.rgb*fragColor.rgb*ubo.colDiffuse.rgb;
    vec3 norm = normalize(fragNormal);

    float lightAccum = 0.08; // ambient floor

    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (ubo.lightsEnabled[i] == 0) continue;

        vec3 lightDir;
        if (ubo.lightsType[i] == 0) {
            // Directional: direction is from position toward target
            lightDir = normalize(ubo.lightsPosition[i] - ubo.lightsTarget[i]);
        } else {
            // Point: direction from surface to light
            lightDir = normalize(ubo.lightsPosition[i] - fragPosition);
        }

        float NdotL = max(dot(norm, lightDir), 0.0);

        // Quantize NdotL into numBands discrete steps
        float quantized = min(floor(NdotL*ubo.numBands), ubo.numBands - 1.0)/(ubo.numBands - 1.0);
        lightAccum += quantized*ubo.lightsColor[i].r;
    }

    lightAccum = clamp(lightAccum, 0.0, 1.0);
    finalColor = vec4(baseColor*lightAccum, texColor.a*ubo.colDiffuse.a);
}
