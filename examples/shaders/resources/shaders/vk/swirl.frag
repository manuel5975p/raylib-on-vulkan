#version 450

// Port of glsl330/swirl.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 center;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

// NOTE: Render size values should be passed from code
const float renderWidth = 800.0;
const float renderHeight = 450.0;

const float radius = 250.0;
const float angle = 0.8;

void main()
{
    vec2 texSize = vec2(renderWidth, renderHeight);
    vec2 tc = fragTexCoord*texSize;
    tc -= ubo.center;

    float dist = length(tc);

    if (dist < radius)
    {
        float percent = (radius - dist)/radius;
        float theta = percent*percent*angle*8.0;
        float s = sin(theta);
        float c = cos(theta);

        tc = vec2(dot(tc, vec2(c, -s)), dot(tc, vec2(s, c)));
    }

    tc += ubo.center;
    vec4 color = texture(texture0, tc/texSize)*ubo.colDiffuse*fragColor;

    finalColor = vec4(color.rgb, 1.0);
}
