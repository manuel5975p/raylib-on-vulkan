#version 450

// Port of glsl330/lighting.vs (fog variant: matching UBO block for fog.frag)

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

#define     MAX_LIGHTS              4
#define     LIGHT_DIRECTIONAL       0
#define     LIGHT_POINT             1

// See lighting.frag for the struct-array flattening note; block identical in both stages
layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec4 ambient;
    vec4 fogColor;
    vec3 viewPos;
    float fogDensity;
    mat4 matNormal;
    int lightsEnabled[MAX_LIGHTS];
    int lightsType[MAX_LIGHTS];
    vec3 lightsPosition[MAX_LIGHTS];
    vec3 lightsTarget[MAX_LIGHTS];
    vec4 lightsColor[MAX_LIGHTS];
} ubo;

// Output vertex attributes (to fragment shader)
layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec3 fragNormal;

void main()
{
    fragPosition = vec3(pc.matModel*vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(ubo.matNormal*vec4(vertexNormal, 1.0)));

    gl_Position = pc.mvp*vec4(vertexPosition, 1.0);
}
