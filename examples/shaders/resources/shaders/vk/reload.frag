#version 450

// Port of glsl330/reload.fs to the rayvulkan shader ABI
// NOTE: gl_FragCoord has a top-left origin on Vulkan, so the mouse y is used as-is

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec2 resolution;    // Viewport resolution (in pixels)
    vec2 mouse;         // Mouse pixel xy coordinates
    float time;         // Total run time (in seconds)
} ubo;

layout(location = 0) out vec4 finalColor;

vec4 DrawCircle(vec2 fragCoord, vec2 position, float radius, vec3 color)
{
    float d = length(position - fragCoord) - radius;
    float t = clamp(d, 0.0, 1.0);
    return vec4(color, 1.0 - t);
}

void main()
{
    vec2 fragCoord = gl_FragCoord.xy;
    vec2 position = ubo.mouse;
    float radius = 40.0;

    // Draw background layer
    vec4 colorA = vec4(0.2, 0.2, 0.8, 1.0);
    vec4 colorB = vec4(1.0, 0.7, 0.2, 1.0);
    vec4 layer1 = mix(colorA, colorB, abs(sin(ubo.time*0.1)));

    // Draw circle layer
    vec3 color = vec3(0.9, 0.16, 0.21);
    vec4 layer2 = DrawCircle(fragCoord, position, radius, color);

    finalColor = mix(layer1, layer2, layer2.a);
}
