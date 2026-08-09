#version 450

// Port of glsl330/lightmap.fs to the rayvulkan shader ABI

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec2 fragTexCoord2;
layout(location = 3) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D texture1;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec4 texelColor2 = texture(texture1, fragTexCoord2);

    finalColor = texelColor*texelColor2;
}
