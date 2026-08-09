#version 450

// Input vertex attributes (from default vertex shader)
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

// Output fragment color
layout(location = 0) out vec4 finalColor;

// All non-sampler uniforms live in one block (rlvk shader ABI)
layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 leftLensCenter;
    vec2 rightLensCenter;
    vec2 leftScreenCenter;
    vec2 rightScreenCenter;
    vec2 scale;
    vec2 scaleIn;
    vec4 deviceWarpParam;
    vec4 chromaAbParam;
};

layout(set = 1, binding = 0) uniform sampler2D texture0;

void main()
{
    // Compute lens distortion
    vec2 lensCenter = fragTexCoord.x < 0.5? leftLensCenter : rightLensCenter;
    vec2 screenCenter = fragTexCoord.x < 0.5? leftScreenCenter : rightScreenCenter;
    vec2 theta = (fragTexCoord - lensCenter)*scaleIn;
    float rSq = theta.x*theta.x + theta.y*theta.y;
    vec2 theta1 = theta*(deviceWarpParam.x + deviceWarpParam.y*rSq + deviceWarpParam.z*rSq*rSq + deviceWarpParam.w*rSq*rSq*rSq);
    vec2 thetaBlue = theta1*(chromaAbParam.z + chromaAbParam.w*rSq);
    vec2 tcBlue = lensCenter + scale*thetaBlue;

    if (any(bvec2(clamp(tcBlue, screenCenter - vec2(0.25, 0.5), screenCenter + vec2(0.25, 0.5)) - tcBlue)))
    {
        // Set black fragment for everything outside the lens border
        finalColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
    else
    {
        // Compute color chroma aberration
        float blue = texture(texture0, tcBlue).b;
        vec2 tcGreen = lensCenter + scale*theta1;
        float green = texture(texture0, tcGreen).g;

        vec2 thetaRed = theta1*(chromaAbParam.x + chromaAbParam.y*rSq);
        vec2 tcRed = lensCenter + scale*thetaRed;

        float red = texture(texture0, tcRed).r;
        finalColor = vec4(red, green, blue, 1.0);
    }
}
