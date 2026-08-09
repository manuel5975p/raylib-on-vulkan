#version 450

#define PI 3.1415926535897932384626433832795

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 offset;            // Offset of the scale
    float zoom;             // Zoom of the scale
    int maxIterations;      // Max iterations per pixel
} ubo;

layout(location = 0) out vec4 finalColor;

const float maxDist = 4.0;              // We consider infinite as 4.0
const float maxDist2 = maxDist*maxDist; // Square of maxDist to avoid computing square root

void main()
{
    vec2 c = vec2((fragTexCoord.x - 0.5)*2.5, (fragTexCoord.y - 0.5)*1.5)/ubo.zoom;
    c.x += ubo.offset.x;
    c.y += ubo.offset.y;
    float a = 0.0;
    float b = 0.0;

    int iter = 0;
    for (iter = 0; iter < ubo.maxIterations; iter++)
    {
        float aa = a*a;
        float bb = b*b;
        if (aa + bb > maxDist2) break;

        float twoab = 2.0*a*b;
        a = aa - bb + c.x;
        b = twoab + c.y;
    }

    if (iter >= ubo.maxIterations) finalColor = vec4(0.0, 0.0, 0.0, 1.0);
    else
    {
        float normR = float(iter%55)/55.0;
        float normG = float(iter%69)/69.0;
        float normB = float(iter%40)/40.0;

        finalColor = vec4(sin(normR*PI), sin(normG*PI), sin(normB*PI), 1.0);
    }
}
