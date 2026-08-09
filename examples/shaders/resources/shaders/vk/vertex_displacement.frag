#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragPosition;
layout(location = 3) in vec3 fragNormal;
layout(location = 4) in float height;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 darkblue = vec4(0.0, 0.13, 0.18, 1.0);
    vec4 lightblue = vec4(1.0, 1.0, 1.0, 1.0);

    // interpolate between two colors based on height
    finalColor = mix(darkblue, lightblue, height);
}
