#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

const float gamma = 0.6;
const float numColors = 8.0;

void main()
{
    vec3 texelColor = texture(texture0, fragTexCoord.xy).rgb;

    texelColor = pow(texelColor, vec3(gamma, gamma, gamma));
    texelColor = texelColor*numColors;
    texelColor = floor(texelColor);
    texelColor = texelColor/numColors;
    texelColor = pow(texelColor, vec3(1.0/gamma));

    finalColor = vec4(texelColor, 1.0);
}
