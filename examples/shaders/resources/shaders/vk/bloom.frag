#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

const vec2 size = vec2(800.0, 450.0);   // Framebuffer size
const float samples = 5.0;              // Pixels per axis; higher = bigger glow, worse performance
const float quality = 2.5;              // Defines size factor: Lower = smaller glow, better quality

void main()
{
    vec4 sum = vec4(0.0);
    vec2 sizeFactor = vec2(1.0)/size*quality;

    vec4 source = texture(texture0, fragTexCoord);

    const int range = 2;            // should be = (samples - 1)/2;

    for (int x = -range; x <= range; x++)
    {
        for (int y = -range; y <= range; y++)
        {
            sum += texture(texture0, fragTexCoord + vec2(x, y)*sizeFactor);
        }
    }

    finalColor = ((sum/(samples*samples)) + source)*ubo.colDiffuse;
}
