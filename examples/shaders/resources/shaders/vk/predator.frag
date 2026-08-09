#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec3 texelColor = texture(texture0, fragTexCoord).rgb;
    vec3 colors[3];
    colors[0] = vec3(0.0, 0.0, 1.0);
    colors[1] = vec3(1.0, 1.0, 0.0);
    colors[2] = vec3(1.0, 0.0, 0.0);

    float lum = (texelColor.r + texelColor.g + texelColor.b)/3.0;

    int ix = (lum < 0.5)? 0:1;

    vec3 tc = mix(colors[ix], colors[ix + 1], (lum - float(ix)*0.5)/0.5);

    finalColor = vec4(tc, 1.0);
}
