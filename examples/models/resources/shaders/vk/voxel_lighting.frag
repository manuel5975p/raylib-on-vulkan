#version 450

// Vulkan port of resources/shaders/glsl330/voxel_lighting.fs

#define MAX_LIGHTS              4
#define LIGHT_DIRECTIONAL       0
#define LIGHT_POINT             1

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragNormal;

// NOTE: see voxel_lighting.vert - the light struct array is flattened
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

layout(location = 0) out vec4 finalColor;

void main()
{
    vec3 lightDot = vec3(0.0);
    vec3 normal = normalize(fragNormal);
    vec3 viewD = normalize(viewPos - fragPosition);
    vec3 specular = vec3(0.0);

    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        if (lightsEnabled[i] == 1)
        {
            vec3 light = vec3(0.0);

            if (lightsType[i] == LIGHT_DIRECTIONAL)
            {
                light = -normalize(lightsTarget[i] - lightsPosition[i]);
            }

            if (lightsType[i] == LIGHT_POINT)
            {
                light = normalize(lightsPosition[i] - fragPosition);
            }

            float NdotL = max(dot(normal, light), 0.0);
            lightDot += lightsColor[i].rgb*NdotL;

            float specCo = 0.0;
            if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(light), normal))), 16.0); // 16 refers to shine
            specular += specCo;
        }
    }

    finalColor = (fragColor*((colDiffuse + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += fragColor*(ambient/10.0)*colDiffuse;

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));
}
