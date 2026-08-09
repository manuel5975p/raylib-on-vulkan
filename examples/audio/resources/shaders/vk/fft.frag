#version 450

// Vulkan port of resources/shaders/glsl330/fft.fs (shader ABI: see src/rlvk.h)

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec2 iResolution;
};

// NOTE: binding 1 (not 0) on purpose: binding 0 is the batch texture slot that
// DrawTextureRec() rebinds on every draw, which would clobber the FFT texture set
// by SetShaderValueTexture(). GL raylib avoids this by assigning sampler uniforms
// to texture units >= 1; on Vulkan the binding is fixed by the shader, so declare it here.
layout(set = 1, binding = 1) uniform sampler2D iChannel0;

layout(location = 0) out vec4 finalColor;

const vec4 BLACK = vec4(0.0, 0.0, 0.0, 1.0);
const vec4 WHITE = vec4(1.0, 1.0, 1.0, 1.0);
const float FFT_ROW = 0.0;
const float NUM_OF_BINS = 512.0;

void main()
{
    vec2 fragCoord = fragTexCoord*iResolution;
    float cellWidth = iResolution.x/NUM_OF_BINS;
    float binIndex = floor(fragCoord.x/cellWidth);
    float localX = mod(fragCoord.x, cellWidth);
    float barWidth  = cellWidth - 1.0;
    vec4 color = WHITE;

    if (localX <= barWidth)
    {
        float sampleX = (binIndex + 0.5)/NUM_OF_BINS;
        vec2  sampleCoord = vec2(sampleX, FFT_ROW);
        float amplitude = texture(iChannel0, sampleCoord).r; // Only filled the red channel, all channels left open for alternative use

        if (fragTexCoord.y < amplitude) color = BLACK;
    }

    finalColor = color;
}
