#version 450

// Port of glsl330/ascii.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec2 resolution;
    float fontSize;     // Fontsize less than 9 may be not complete
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

float GreyScale(in vec3 col)
{
    return dot(col, vec3(0.2126, 0.7152, 0.0722));
}

float GetCharacter(int n, vec2 p)
{
    p = floor(p*vec2(-4.0, 4.0) + 2.5);

    // Check if the coordinate is inside the 5x5 grid (0 to 4)
    if (clamp(p.x, 0.0, 4.0) == p.x && clamp(p.y, 0.0, 4.0) == p.y)
    {
        int a = int(round(p.x) + 5.0*round(p.y));
        if (((n >> a) & 1) == 1)
        {
            return 1.0;
        }
    }

    return 0.0; // The bit is off, or we are outside the grid
}

void main()
{
    vec2 charPixelSize = vec2(ubo.fontSize, ubo.fontSize);
    vec2 uvCellSize = charPixelSize/ubo.resolution;

    vec2 cellUV = floor(fragTexCoord/uvCellSize)*uvCellSize;

    vec3 cellColor = texture(texture0, cellUV).rgb;

    float gray = GreyScale(cellColor);

    int n = 4096;

    if (gray > 0.2) n = 65600;    // :
    if (gray > 0.3) n = 18725316; // v
    if (gray > 0.4) n = 15255086; // o
    if (gray > 0.5) n = 13121101; // &
    if (gray > 0.6) n = 15252014; // 8
    if (gray > 0.7) n = 13195790; // @
    if (gray > 0.8) n = 11512810; // #

    vec2 localUV = (fragTexCoord - cellUV)/uvCellSize;

    vec2 p = localUV*2.0 - 1.0;

    vec3 color = cellColor*GetCharacter(n, p);

    finalColor = vec4(color, 1.0);
}
