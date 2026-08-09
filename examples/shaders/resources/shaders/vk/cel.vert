#version 450

// Port of glsl330/cel.vs to the rayvulkan shader ABI

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

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

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec3 fragNormal;

void main() {
    fragPosition = vec3(pc.matModel*vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(ubo.matNormal*vec4(vertexNormal, 0.0)));
    gl_Position = pc.mvp*vec4(vertexPosition, 1.0);
}
