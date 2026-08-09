#version 450

// Port of glsl330/hybrid_raster.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);

    finalColor = texelColor*ubo.colDiffuse*fragColor;
    gl_FragDepth = finalColor.z;
}
