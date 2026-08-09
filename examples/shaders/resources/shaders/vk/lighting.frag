#version 450

// Port of glsl330/lighting.fs to the rayvulkan shader ABI
// NOTE: The GLSL "Light lights[MAX_LIGHTS]" struct array is flattened into parallel
// arrays because uniform locations resolve to UBO member names (see rlights.h)

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec3 fragNormal;

#define     MAX_LIGHTS              4
#define     LIGHT_DIRECTIONAL       0
#define     LIGHT_POINT             1

// NOTE: The GLSL "Light lights[MAX_LIGHTS]" struct array is flattened into parallel
// arrays because uniform locations resolve to plain UBO member names (see rlights.h).
// The block must be declared IDENTICALLY in both stages (single set 0 binding 0).
layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec4 ambient;
    vec3 viewPos;
    mat4 matNormal;
    int lightsEnabled[MAX_LIGHTS];
    int lightsType[MAX_LIGHTS];
    vec3 lightsPosition[MAX_LIGHTS];
    vec3 lightsTarget[MAX_LIGHTS];
    vec4 lightsColor[MAX_LIGHTS];
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 lightDot = vec3(0.0);
    vec3 normal = normalize(fragNormal);
    vec3 viewD = normalize(ubo.viewPos - fragPosition);
    vec3 specular = vec3(0.0);

    vec4 tint = ubo.colDiffuse*fragColor;

    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        if (ubo.lightsEnabled[i] == 1)
        {
            vec3 light = vec3(0.0);

            if (ubo.lightsType[i] == LIGHT_DIRECTIONAL)
            {
                light = -normalize(ubo.lightsTarget[i] - ubo.lightsPosition[i]);
            }

            if (ubo.lightsType[i] == LIGHT_POINT)
            {
                light = normalize(ubo.lightsPosition[i] - fragPosition);
            }

            float NdotL = max(dot(normal, light), 0.0);
            lightDot += ubo.lightsColor[i].rgb*NdotL;

            float specCo = 0.0;
            if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(light), normal))), 16.0); // 16 refers to shine
            specular += specCo;
        }
    }

    finalColor = (texelColor*((tint + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += texelColor*(ubo.ambient/10.0)*tint;

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));
}
