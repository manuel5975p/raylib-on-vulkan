#version 450

const int MAX_INDEXED_COLORS = 8;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    ivec3 palette[MAX_INDEXED_COLORS];
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    // NOTE: The texel is actually the GRAYSCALE index color
    vec4 texelColor = texture(texture0, fragTexCoord)*fragColor;

    int index = int(texelColor.r*255.0);
    ivec3 color = ubo.palette[index];

    finalColor = vec4(vec3(color)/255.0, texelColor.a);
}
