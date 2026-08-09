#version 450

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    float time;
} ubo;

layout(set = 1, binding = 1) uniform sampler2D perlinNoiseMap;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragPosition;
layout(location = 3) out vec3 fragNormal;
layout(location = 4) out float height;

void main()
{
    // Calculate animated texture coordinates based on time and vertex position
    vec2 animatedTexCoord = sin(vertexTexCoord + vec2(sin(ubo.time + vertexPosition.x*0.1), cos(ubo.time + vertexPosition.z*0.1))*0.3);

    // Normalize animated texture coordinates to range [0, 1]
    animatedTexCoord = animatedTexCoord*0.5 + 0.5;

    // Fetch displacement from the perlin noise map
    float displacement = textureLod(perlinNoiseMap, animatedTexCoord, 0.0).r*7.0; // Amplified displacement

    vec3 displacedPosition = vertexPosition + vec3(0.0, displacement, 0.0);

    fragPosition = vec3(pc.matModel*vec4(displacedPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(ubo.matNormal*vec4(vertexNormal, 1.0)));
    height = displacedPosition.y*0.2; // send height to fragment shader for coloring

    gl_Position = pc.mvp*vec4(displacedPosition, 1.0);
}
