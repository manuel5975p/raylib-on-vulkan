#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 size;
    float seconds;
    float freqX;
    float freqY;
    float ampX;
    float ampY;
    float speedX;
    float speedY;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    float pixelWidth = 1.0/ubo.size.x;
    float pixelHeight = 1.0/ubo.size.y;
    float aspect = pixelHeight/pixelWidth;
    float boxLeft = 0.0;
    float boxTop = 0.0;

    vec2 p = fragTexCoord;
    p.x += cos((fragTexCoord.y - boxTop)*ubo.freqX/(pixelWidth*750.0) + (ubo.seconds*ubo.speedX))*ubo.ampX*pixelWidth;
    p.y += sin((fragTexCoord.x - boxLeft)*ubo.freqY*aspect/(pixelHeight*750.0) + (ubo.seconds*ubo.speedY))*ubo.ampY*pixelHeight;

    finalColor = texture(texture0, p)*ubo.colDiffuse*fragColor;
}
