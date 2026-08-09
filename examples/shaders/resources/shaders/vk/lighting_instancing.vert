#version 450

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 8) in mat4 instanceTransform;     // occupies locations 8, 9, 10, 11

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matNormal;
    vec4 ambient;
    vec4 lightColor;
    vec3 lightPosition;
    vec3 lightTarget;
    vec3 viewPos;
    int lightType;
    int lightEnabled;
} ubo;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragPosition;
layout(location = 3) out vec3 fragNormal;

void main()
{
    fragPosition = vec3(instanceTransform*vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(1.0);
    fragNormal = normalize(vec3(ubo.matNormal*vec4(vertexNormal, 1.0)));

    gl_Position = pc.mvp*instanceTransform*vec4(vertexPosition, 1.0);
}
