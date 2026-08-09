#version 450

// Vulkan port of resources/shaders/glsl330/alpha_discard.fs (shader ABI: see src/rlvk.h)

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
};

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    if (texelColor.a == 0.0) discard;
    finalColor = texelColor*fragColor*colDiffuse;
}
