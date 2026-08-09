#version 450

// Port of glsl330/fog.fs to the rayvulkan shader ABI (paired with lighting.vert)

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec3 fragNormal;

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

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 lightDot = vec3(0.0);
    vec3 normal = normalize(fragNormal);
    vec3 viewD = normalize(ubo.viewPos - fragPosition);
    vec3 specular = vec3(0.0);

    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        if (ubo.lightsEnabled[i] == 1)
        {
            vec3 light = vec3(0.0);

            if (ubo.lightsType[i] == LIGHT_DIRECTIONAL) light = -normalize(ubo.lightsTarget[i] - ubo.lightsPosition[i]);
            if (ubo.lightsType[i] == LIGHT_POINT) light = normalize(ubo.lightsPosition[i] - fragPosition);

            float NdotL = max(dot(normal, light), 0.0);
            lightDot += ubo.lightsColor[i].rgb*NdotL;

            float specCo = 0.0;
            if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(light), normal))), 16.0); // Shine: 16.0
            specular += specCo;
        }
    }

    finalColor = (texelColor*((ubo.colDiffuse + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += texelColor*(ubo.ambient/10.0);

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));

    // Fog calculation
    float dist = length(ubo.viewPos - fragPosition);

    // Exponential fog
    float fogFactor = 1.0/exp((dist*ubo.fogDensity)*(dist*ubo.fogDensity));

    fogFactor = clamp(fogFactor, 0.0, 1.0);

    finalColor = mix(ubo.fogColor, finalColor, fogFactor);
}
