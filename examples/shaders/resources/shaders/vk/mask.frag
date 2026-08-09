#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    int frame;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D mask;

layout(location = 0) out vec4 finalColor;

void main()
{
    float f = float(ubo.frame);
    vec4 maskColour = texture(mask, fragTexCoord + vec2(sin(-f/150.0)/10.0, cos(-f/170.0)/10.0));
    if (maskColour.r < 0.25) discard;
    vec4 texelColor = texture(texture0, fragTexCoord + vec2(sin(f/90.0)/8.0, cos(f/60.0)/8.0));

    finalColor = texelColor*maskColour;
}
