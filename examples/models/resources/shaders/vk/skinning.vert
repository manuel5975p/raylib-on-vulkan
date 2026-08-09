#version 450

// Vulkan port of resources/shaders/glsl330/skinning.vs
// Bone attributes follow the rlvk ABI: location 6 uvec4 vertexBoneIds,
// location 7 vec4 vertexBoneWeights. Bone palette is uploaded by rmodels into
// the "boneMatrices" UBO member (SHADER_LOC_MATRIX_BONETRANSFORMS).

#define MAX_BONE_NUM 128

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;
layout(location = 6) in uvec4 vertexBoneIds;
layout(location = 7) in vec4 vertexBoneWeights;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    mat4 boneMatrices[MAX_BONE_NUM];
};

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
};

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragNormal;

void main()
{
    int boneIndex0 = int(vertexBoneIds.x);
    int boneIndex1 = int(vertexBoneIds.y);
    int boneIndex2 = int(vertexBoneIds.z);
    int boneIndex3 = int(vertexBoneIds.w);

    vec4 skinnedPosition =
        vertexBoneWeights.x*(boneMatrices[boneIndex0]*vec4(vertexPosition, 1.0)) +
        vertexBoneWeights.y*(boneMatrices[boneIndex1]*vec4(vertexPosition, 1.0)) +
        vertexBoneWeights.z*(boneMatrices[boneIndex2]*vec4(vertexPosition, 1.0)) +
        vertexBoneWeights.w*(boneMatrices[boneIndex3]*vec4(vertexPosition, 1.0));

    vec4 skinnedNormal =
        vertexBoneWeights.x*(boneMatrices[boneIndex0]*vec4(vertexNormal, 0.0)) +
        vertexBoneWeights.y*(boneMatrices[boneIndex1]*vec4(vertexNormal, 0.0)) +
        vertexBoneWeights.z*(boneMatrices[boneIndex2]*vec4(vertexNormal, 0.0)) +
        vertexBoneWeights.w*(boneMatrices[boneIndex3]*vec4(vertexNormal, 0.0));
    skinnedNormal.w = 0.0;

    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal*skinnedNormal));

    gl_Position = mvp*skinnedPosition;
}
