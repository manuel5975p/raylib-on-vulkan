#version 450

// Port of glsl330/deferred_shading.fs to the rayvulkan shader ABI
// The light struct array is flattened into parallel arrays (see rlights.h)

#define NR_LIGHTS 4

layout(location = 0) in vec2 texCoord;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec3 viewPosition;
    int lightsEnabled[NR_LIGHTS];
    int lightsType[NR_LIGHTS];      // Unused in this demo
    vec3 lightsPosition[NR_LIGHTS];
    vec3 lightsTarget[NR_LIGHTS];   // Unused in this demo
    vec4 lightsColor[NR_LIGHTS];
} ubo;

layout(set = 1, binding = 0) uniform sampler2D gPosition;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D gAlbedoSpec;

layout(location = 0) out vec4 finalColor;

const float QUADRATIC = 0.032;
const float LINEAR = 0.09;

void main()
{
    vec3 fragPosition = texture(gPosition, texCoord).rgb;
    vec3 normal = texture(gNormal, texCoord).rgb;
    vec3 albedo = texture(gAlbedoSpec, texCoord).rgb;
    float specular = texture(gAlbedoSpec, texCoord).a;

    vec3 ambient = albedo*vec3(0.1);
    vec3 viewDirection = normalize(ubo.viewPosition - fragPosition);

    for (int i = 0; i < NR_LIGHTS; i++)
    {
        if (ubo.lightsEnabled[i] == 0) continue;
        vec3 lightDirection = ubo.lightsPosition[i] - fragPosition;
        vec3 diffuse = max(dot(normal, lightDirection), 0.0)*albedo*ubo.lightsColor[i].xyz;

        vec3 halfwayDirection = normalize(lightDirection + viewDirection);
        float spec = pow(max(dot(normal, halfwayDirection), 0.0), 32.0);
        vec3 specularColor = specular*spec*ubo.lightsColor[i].xyz;

        // Attenuation
        float dist = length(ubo.lightsPosition[i] - fragPosition);
        float attenuation = 1.0/(1.0 + LINEAR*dist + QUADRATIC*dist*dist);
        diffuse *= attenuation;
        specularColor *= attenuation;
        ambient += diffuse + specularColor;
    }

    finalColor = vec4(ambient, 1.0);
}
