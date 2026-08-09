#version 450

// Vulkan port of resources/shaders/glsl330/skinning.fs

#define MAX_BONE_NUM 128

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragNormal;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    mat4 boneMatrices[MAX_BONE_NUM];
};

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    finalColor = texelColor*colDiffuse*fragColor;
}
