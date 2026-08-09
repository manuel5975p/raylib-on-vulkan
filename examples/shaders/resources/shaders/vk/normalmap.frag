#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragPosition;
layout(location = 3) in vec3 fragNormal;    // used for when normal mapping is toggled off
layout(location = 4) in mat3 TBN;           // occupies locations 4, 5, 6

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec4 tintColor;
    vec3 viewPos;
    vec3 lightPos;
    float specularExponent;
    int useNormalMap;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D normalMap;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, vec2(fragTexCoord.x, fragTexCoord.y));
    vec3 specular = vec3(0.0);
    vec3 viewDir = normalize(ubo.viewPos - fragPosition);
    vec3 lightDir = normalize(ubo.lightPos - fragPosition);

    vec3 normal;
    if (ubo.useNormalMap != 0)
    {
        normal = texture(normalMap, vec2(fragTexCoord.x, fragTexCoord.y)).rgb;

        // Transform normal values to the range -1.0 ... 1.0
        normal = normalize(normal*2.0 - 1.0);

        // Transform the normal from tangent-space to world-space for lighting calculation
        normal = normalize(normal*TBN);
    }
    else normal = normalize(fragNormal);

    vec4 tint = ubo.colDiffuse*fragColor;

    vec3 lightColor = vec3(1.0, 1.0, 1.0);
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 lightDot = lightColor*NdotL;

    float specCo = 0.0;

    if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewDir, reflect(-lightDir, normal))), ubo.specularExponent);

    specular += specCo;

    finalColor = (texelColor*((tint + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += texelColor*(vec4(1.0, 1.0, 1.0, 1.0)/40.0)*tint;

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));
}
