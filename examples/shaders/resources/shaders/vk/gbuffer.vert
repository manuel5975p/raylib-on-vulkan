#version 450

// Port of glsl330/gbuffer.vs to the rayvulkan shader ABI

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matView;
    mat4 matProjection;
} ubo;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragNormal;

void main()
{
    vec4 worldPos = pc.matModel*vec4(vertexPosition, 1.0);
    fragPosition = worldPos.xyz;
    fragTexCoord = vertexTexCoord;

    mat3 normalMatrix = transpose(inverse(mat3(pc.matModel)));
    fragNormal = normalMatrix*vertexNormal;

    gl_Position = ubo.matProjection*ubo.matView*worldPos;
}
