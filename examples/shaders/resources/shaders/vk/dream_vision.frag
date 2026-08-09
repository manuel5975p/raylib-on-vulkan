#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 vertColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = texture(texture0, fragTexCoord);

    color += texture(texture0, fragTexCoord + 0.001);
    color += texture(texture0, fragTexCoord + 0.003);
    color += texture(texture0, fragTexCoord + 0.005);
    color += texture(texture0, fragTexCoord + 0.007);
    color += texture(texture0, fragTexCoord + 0.009);
    color += texture(texture0, fragTexCoord + 0.011);

    color += texture(texture0, fragTexCoord - 0.001);
    color += texture(texture0, fragTexCoord - 0.003);
    color += texture(texture0, fragTexCoord - 0.005);
    color += texture(texture0, fragTexCoord - 0.007);
    color += texture(texture0, fragTexCoord - 0.009);
    color += texture(texture0, fragTexCoord - 0.011);

    color.rgb = vec3((color.r + color.g + color.b)/3.0);
    color = color/9.5;

    fragColor = color;
}
