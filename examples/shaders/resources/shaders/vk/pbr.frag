#version 450

// Port of glsl330/pbr.fs to the rayvulkan shader ABI
// The "Light lights[MAX_LIGHTS]" struct array is flattened into parallel arrays
// because uniform locations resolve to plain UBO member names

#define MAX_LIGHTS              4
#define LIGHT_DIRECTIONAL       0
#define LIGHT_POINT             1
#define PI 3.14159265358979323846

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec3 fragNormal;
layout(location = 4) in mat3 TBN;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 albedoColor;           // Bound to SHADER_LOC_COLOR_DIFFUSE by the example
    vec4 emissiveColor;
    vec3 viewPos;
    float normalValue;
    vec3 ambientColor;
    float ambient;
    vec2 tiling;
    vec2 offset;
    int numOfLights;
    int useTexAlbedo;
    int useTexNormal;
    int useTexMRA;
    int useTexEmissive;
    float metallicValue;
    float roughnessValue;
    float aoValue;
    float emissivePower;
    int lightsEnabled[MAX_LIGHTS];
    int lightsType[MAX_LIGHTS];
    vec3 lightsPosition[MAX_LIGHTS];
    vec3 lightsTarget[MAX_LIGHTS];
    vec4 lightsColor[MAX_LIGHTS];
    float lightsIntensity[MAX_LIGHTS];
} ubo;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D mraMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;  // r: height  g: emissive

layout(location = 0) out vec4 finalColor;

// Reflectivity in range 0.0 to 1.0
vec3 SchlickFresnel(float hDotV, vec3 refl)
{
    return refl + (1.0 - refl)*pow(1.0 - hDotV, 5.0);
}

float GgxDistribution(float nDotH, float roughness)
{
    float a = roughness*roughness*roughness*roughness;
    float d = nDotH*nDotH*(a - 1.0) + 1.0;
    d = PI*d*d;
    return (a/max(d, 0.0000001));
}

float GeomSmith(float nDotV, float nDotL, float roughness)
{
    float r = roughness + 1.0;
    float k = r*r/8.0;
    float ik = 1.0 - k;
    float ggx1 = nDotV/(nDotV*ik + k);
    float ggx2 = nDotL/(nDotL*ik + k);
    return ggx1*ggx2;
}

vec3 ComputePBR()
{
    vec2 uv = vec2(fragTexCoord.x*ubo.tiling.x + ubo.offset.x, fragTexCoord.y*ubo.tiling.y + ubo.offset.y);

    vec3 albedo = texture(albedoMap, uv).rgb;
    albedo = vec3(ubo.albedoColor.x*albedo.x, ubo.albedoColor.y*albedo.y, ubo.albedoColor.z*albedo.z);

    float metallic = clamp(ubo.metallicValue, 0.0, 1.0);
    float roughness = clamp(ubo.roughnessValue, 0.0, 1.0);
    float ao = clamp(ubo.aoValue, 0.0, 1.0);

    if (ubo.useTexMRA == 1)
    {
        vec4 mra = texture(mraMap, uv);
        metallic = clamp(mra.r + ubo.metallicValue, 0.04, 1.0);
        roughness = clamp(mra.g + ubo.roughnessValue, 0.04, 1.0);
        ao = (mra.b + ubo.aoValue)*0.5;
    }

    vec3 N = normalize(fragNormal);
    if (ubo.useTexNormal == 1)
    {
        N = texture(normalMap, uv).rgb;
        N = normalize(N*2.0 - 1.0);
        N = normalize(N*TBN);
    }

    vec3 V = normalize(ubo.viewPos - fragPosition);

    vec3 emissive = (texture(emissiveMap, uv).rgb).g*ubo.emissiveColor.rgb*ubo.emissivePower*float(ubo.useTexEmissive);

    // If dia-electric use base reflectivity of 0.04, otherwise it is a metal: use albedo
    vec3 baseRefl = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 lightAccum = vec3(0.0);

    for (int i = 0; i < ubo.numOfLights; i++)
    {
        vec3 L = normalize(ubo.lightsPosition[i] - fragPosition);
        vec3 H = normalize(V + L);
        float dist = length(ubo.lightsPosition[i] - fragPosition);
        float attenuation = 1.0/(dist*dist*0.23);
        vec3 radiance = ubo.lightsColor[i].rgb*ubo.lightsIntensity[i]*attenuation;

        float nDotV = max(dot(N, V), 0.0000001);
        float nDotL = max(dot(N, L), 0.0000001);
        float hDotV = max(dot(H, V), 0.0);
        float nDotH = max(dot(N, H), 0.0);
        float D = GgxDistribution(nDotH, roughness);
        float G = GeomSmith(nDotV, nDotL, roughness);
        vec3 F = SchlickFresnel(hDotV, baseRefl);

        vec3 spec = (D*G*F)/(4.0*nDotV*nDotL);

        vec3 kD = vec3(1.0) - F;
        kD *= 1.0 - metallic;
        lightAccum += ((kD*albedo.rgb/PI + spec)*radiance*nDotL)*float(ubo.lightsEnabled[i]);
    }

    vec3 ambientFinal = (ubo.ambientColor + albedo)*ubo.ambient*0.5;

    return (ambientFinal + lightAccum*ao + emissive);
}

void main()
{
    vec3 color = ComputePBR();

    // HDR tonemapping
    color = pow(color, color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0/2.2));

    finalColor = vec4(color, 1.0);
}
