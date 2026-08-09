#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

// NOTE: Render size values must be passed from code
const float renderWidth = 800.0;
const float renderHeight = 450.0;

const float stitchingSize = 6.0;

// NOTE: Uniform initializers are not allowed in Vulkan GLSL blocks, kept as constant
const int invert = 0;

vec4 PostFX(sampler2D tex, vec2 uv)
{
    vec4 c = vec4(0.0);
    float size = stitchingSize;
    vec2 cPos = uv*vec2(renderWidth, renderHeight);
    vec2 tlPos = floor(cPos/vec2(size, size));
    tlPos *= size;

    int remX = int(mod(cPos.x, size));
    int remY = int(mod(cPos.y, size));

    if (remX == 0 && remY == 0) tlPos = cPos;

    vec2 blPos = tlPos;
    blPos.y += (size - 1.0);

    if ((remX == remY) || (((int(cPos.x) - int(blPos.x)) == (int(blPos.y) - int(cPos.y)))))
    {
        if (invert == 1) c = vec4(0.2, 0.15, 0.05, 1.0);
        else c = texture(tex, tlPos*vec2(1.0/renderWidth, 1.0/renderHeight))*1.4;
    }
    else
    {
        if (invert == 1) c = texture(tex, tlPos*vec2(1.0/renderWidth, 1.0/renderHeight))*1.4;
        else c = vec4(0.0, 0.0, 0.0, 1.0);
    }

    return c;
}

void main()
{
    vec3 tc = PostFX(texture0, fragTexCoord).rgb;

    finalColor = vec4(tc, 1.0);
}
