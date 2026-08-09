#version 450

// Vulkan port of resources/shaders/glsl330/voxel_lighting.vs

#define MAX_LIGHTS 4

layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

// NOTE: rlvk reflects only top-level UBO members, so the upstream
// "Light lights[MAX_LIGHTS]" struct array is flattened into parallel arrays
// (rlights.h in this directory resolves the matching names)
layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec4 ambient;
    mat4 matNormal;
    vec3 viewPos;
    int lightsEnabled[MAX_LIGHTS];
    int lightsType[MAX_LIGHTS];
    vec3 lightsPosition[MAX_LIGHTS];
    vec3 lightsTarget[MAX_LIGHTS];
    vec4 lightsColor[MAX_LIGHTS];
};

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
};

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragNormal;

void main()
{
    // Send vertex attributes to fragment shader
    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 1.0)));

    // Calculate final vertex position
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
