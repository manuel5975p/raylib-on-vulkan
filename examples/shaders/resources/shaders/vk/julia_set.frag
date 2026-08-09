#version 450

// Port of glsl330/julia_set.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec2 c;             // c.x = real, c.y = imaginary component. Equation done is z^2 + c
    vec2 offset;        // Offset of the scale
    float zoom;         // Zoom of the scale
} ubo;

layout(location = 0) out vec4 finalColor;

const int maxIterations = 255;  // Max iterations to do
const float colorCycles = 2.0;  // Number of times the color palette repeats

vec2 ComplexSquare(vec2 z)
{
    return vec2(z.x*z.x - z.y*z.y, z.x*z.y*2.0);
}

vec3 Hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz)*6.0 - K.www);
    return c.z*mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main()
{
    vec2 z = vec2((fragTexCoord.x - 0.5)*2.5, (fragTexCoord.y - 0.5)*1.5)/ubo.zoom;
    z.x += ubo.offset.x;
    z.y += ubo.offset.y;

    int iterations = 0;
    for (iterations = 0; iterations < maxIterations; iterations++)
    {
        z = ComplexSquare(z) + ubo.c;

        if (dot(z, z) > 4.0) break;
    }

    z = ComplexSquare(z) + ubo.c;
    z = ComplexSquare(z) + ubo.c;

    float smoothVal = float(iterations) + 1.0 - (log(log(length(z)))/log(2.0));

    float norm = smoothVal/float(maxIterations);

    if (norm > 0.999) finalColor = vec4(0.0, 0.0, 0.0, 1.0);
    else finalColor = vec4(Hsv2rgb(vec3(norm*colorCycles, 1.0, 1.0)), 1.0);
}
