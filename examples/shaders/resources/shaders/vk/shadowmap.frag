#version 450

// This shader is based on the basic lighting shader
// This only supports one light, which is directional, and it (of course) supports shadows

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragPosition;
layout(location = 3) in vec3 fragNormal;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    mat4 lightVP;               // Light source view-projection matrix
    vec4 lightColor;
    vec4 ambient;
    vec3 lightDir;
    vec3 viewPos;
    int shadowMapResolution;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D shadowMap;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 lightDot = vec3(0.0);
    vec3 normal = normalize(fragNormal);
    vec3 viewD = normalize(ubo.viewPos - fragPosition);
    vec3 specular = vec3(0.0);

    vec3 l = -ubo.lightDir;

    float NdotL = max(dot(normal, l), 0.0);
    lightDot += ubo.lightColor.rgb*NdotL;

    float specCo = 0.0;
    if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(l), normal))), 16.0); // 16 refers to shine
    specular += specCo;

    finalColor = (texelColor*((ubo.colDiffuse + vec4(specular, 1.0))*vec4(lightDot, 1.0)));

    // Shadow calculations
    vec4 fragPosLightSpace = ubo.lightVP*vec4(fragPosition, 1.0);
    fragPosLightSpace.xyz /= fragPosLightSpace.w;               // Perform the perspective division
    // NOTE: rlvk keeps GL-convention user matrices (rlGetMatrixProjection) and applies the
    // GL->Vulkan clip correction (z' = 0.5z + 0.5) itself, so the upstream remap still holds
    fragPosLightSpace.xyz = (fragPosLightSpace.xyz + 1.0)/2.0; // Transform from [-1, 1] range to [0, 1] range
    vec2 sampleCoords = fragPosLightSpace.xy;
    float curDepth = fragPosLightSpace.z;

    // Slope-scale depth bias reduces "shadow acne" artifacts
    float bias = max(0.0002*(1.0 - dot(normal, l)), 0.00002) + 0.00001;
    int shadowCounter = 0;
    const int numSamples = 9;

    // PCF (percentage-closer filtering)
    vec2 texelSize = vec2(1.0/float(ubo.shadowMapResolution));
    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            float sampleDepth = texture(shadowMap, sampleCoords + texelSize*vec2(x, y)).r;
            if (curDepth - bias > sampleDepth) shadowCounter++;
        }
    }
    finalColor = mix(finalColor, vec4(0.0, 0.0, 0.0, 1.0), float(shadowCounter)/float(numSamples));

    // Add ambient lighting whether in shadow or not
    finalColor += texelColor*(ubo.ambient/10.0)*ubo.colDiffuse;

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));
}
