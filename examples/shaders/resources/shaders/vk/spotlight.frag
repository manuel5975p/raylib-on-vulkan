#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 finalColor;

#define MAX_SPOTS   3

// NOTE: rayvulkan reflection resolves top-level UBO members only, so the upstream
// "Spot spots[]" struct array is flattened into parallel arrays here
layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 spotPos[MAX_SPOTS];        // window coords of spot
    float spotInner[MAX_SPOTS];     // inner fully transparent centre radius
    float spotRadius[MAX_SPOTS];    // alpha fades out to this radius
    float screenWidth;              // Width of the screen
} ubo;

void main()
{
    float alpha = 1.0;

    // Get the position of the current fragment (screen coordinates!)
    vec2 pos = vec2(gl_FragCoord.x, gl_FragCoord.y);

    // Find out which spotlight is nearest
    float d = 65000.0;  // some high value
    int fi = -1;        // found index

    for (int i = 0; i < MAX_SPOTS; i++)
    {
        for (int j = 0; j < MAX_SPOTS; j++)
        {
            float dj = distance(pos, ubo.spotPos[j]) - ubo.spotRadius[j] + ubo.spotRadius[i];

            if (d > dj)
            {
                d = dj;
                fi = i;
            }
        }
    }

    // d now equals distance to nearest spot...
    if (fi != -1)
    {
        if (d > ubo.spotRadius[fi]) alpha = 1.0;
        else
        {
            if (d < ubo.spotInner[fi]) alpha = 0.0;
            else alpha = (d - ubo.spotInner[fi])/(ubo.spotRadius[fi] - ubo.spotInner[fi]);
        }
    }

    // Right hand side of screen is dimly lit
    if ((pos.x > ubo.screenWidth/2.0) && (alpha > 0.9)) alpha = 0.9;

    finalColor = vec4(0.0, 0.0, 0.0, alpha);
}
