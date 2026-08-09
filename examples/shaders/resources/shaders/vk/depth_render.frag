#version 450

// Port of glsl330/depth_render.fs to the rayvulkan shader ABI
// NOTE: "bool flipY" becomes an int (bools are not usable through SetShaderValue)

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    int flipY;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D depthTexture;

layout(location = 0) out vec4 finalColor;

const float nearPlane = 0.1;
const float farPlane = 100.0;

void main()
{
    // Handle potential Y-flipping
    vec2 texCoord = fragTexCoord;
    if (ubo.flipY != 0) texCoord.y = 1.0 - texCoord.y;

    // Sample depth
    float depth = texture(depthTexture, texCoord).r;

    // Linearize depth value
    float linearDepth = (2.0*nearPlane)/(farPlane + nearPlane - depth*(farPlane - nearPlane));

    finalColor = vec4(vec3(linearDepth), 1.0);
}
