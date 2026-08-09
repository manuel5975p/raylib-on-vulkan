#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    float divider;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D texture1;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor0 = texture(texture0, fragTexCoord);
    vec4 texelColor1 = texture(texture1, fragTexCoord);

    float x = fract(fragTexCoord.s);
    float final = smoothstep(ubo.divider - 0.1, ubo.divider + 0.1, x);

    finalColor = mix(texelColor0, texelColor1, final);
}
