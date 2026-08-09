#version 450

// Port of glsl330/color_correction.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    float contrast;
    float saturation;
    float brightness;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texel = texture(texture0, fragTexCoord);   // Get texel color

    // Apply contrast
    texel.rgb = (texel.rgb - 0.5)*(ubo.contrast/100.0 + 1.0) + 0.5;

    // Apply brightness
    texel.rgb = texel.rgb + ubo.brightness/100.0;

    // Apply saturation
    float intensity = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
    texel.rgb = (texel.rgb - intensity)*ubo.saturation/100.0 + texel.rgb;

    finalColor = texel;
}
