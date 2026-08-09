#version 450

// Single-light variant of the raylib lighting shader used by the instancing example.
// NOTE: rayvulkan uniform reflection resolves top-level UBO members only, so the
// upstream "Light lights[]" struct array is flattened to scalar uniforms here

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragPosition;
layout(location = 3) in vec3 fragNormal;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    vec4 ambient;
    vec4 lightColor;
    vec3 lightPosition;
    vec3 lightTarget;
    vec3 viewPos;
    int lightType;          // 0 = directional, 1 = point
    int lightEnabled;
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

    if (ubo.lightEnabled == 1)
    {
        vec3 light = vec3(0.0);

        if (ubo.lightType == 0) light = -normalize(ubo.lightTarget - ubo.lightPosition);
        else light = normalize(ubo.lightPosition - fragPosition);

        float NdotL = max(dot(normal, light), 0.0);
        lightDot += ubo.lightColor.rgb*NdotL;

        float specCo = 0.0;
        if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(light), normal))), 16.0); // 16 refers to shine
        specular += specCo;
    }

    finalColor = (texelColor*((tint + vec4(specular, 1.0))*vec4(lightDot, 1.0)));
    finalColor += texelColor*(ubo.ambient/10.0)*tint;

    // Gamma correction
    finalColor = pow(finalColor, vec4(1.0/2.2));
}
