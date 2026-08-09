#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

const float hatchOffsetY = 5.0;
const float lumThreshold01 = 0.9;
const float lumThreshold02 = 0.7;
const float lumThreshold03 = 0.5;
const float lumThreshold04 = 0.3;

void main()
{
    vec3 tc = vec3(1.0, 1.0, 1.0);
    float lum = length(texture(texture0, fragTexCoord).rgb);

    if (lum < lumThreshold01)
    {
        if (mod(gl_FragCoord.x + gl_FragCoord.y, 10.0) == 0.0) tc = vec3(0.0, 0.0, 0.0);
    }

    if (lum < lumThreshold02)
    {
        if (mod(gl_FragCoord.x - gl_FragCoord.y, 10.0) == 0.0) tc = vec3(0.0, 0.0, 0.0);
    }

    if (lum < lumThreshold03)
    {
        if (mod(gl_FragCoord.x + gl_FragCoord.y - hatchOffsetY, 10.0) == 0.0) tc = vec3(0.0, 0.0, 0.0);
    }

    if (lum < lumThreshold04)
    {
        if (mod(gl_FragCoord.x - gl_FragCoord.y - hatchOffsetY, 10.0) == 0.0) tc = vec3(0.0, 0.0, 0.0);
    }

    finalColor = vec4(tc, 1.0);
}
