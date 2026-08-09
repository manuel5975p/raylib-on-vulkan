#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

// NOTE: Render size values must be passed from code
const float renderWidth = 800.0;
const float renderHeight = 450.0;
const float offset = 0.0;

void main()
{
    float frequency = renderHeight/3.0;

    // Scanlines method 2
    float globalPos = (fragTexCoord.y + offset)*frequency;
    float wavePos = cos((fract(globalPos) - 0.5)*3.14);

    vec4 texelColor = texture(texture0, fragTexCoord);

    finalColor = mix(vec4(0.0, 0.3, 0.0, 0.0), texelColor, wavePos);
}
