#version 450

// rayvulkan default batch vertex shader (shader ABI: see rlvk.h)

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = pc.mvp*vec4(vertexPosition, 1.0);
    gl_PointSize = 1.0;     // Required for POINT_LIST pipelines (VUID-...-topology-08773)
}
