#version 450

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;
layout(location = 4) in vec4 vertexTangent;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragPosition;
layout(location = 3) out vec3 fragNormal;   // used for when normal mapping is toggled off
layout(location = 4) out mat3 TBN;          // occupies locations 4, 5, 6

void main()
{
    // Compute binormal from vertex normal and tangent. W component is the tangent handedness
    vec3 vertexBinormal = cross(vertexNormal, vertexTangent.xyz)*vertexTangent.w;

    mat3 normalMatrix = transpose(inverse(mat3(pc.matModel)));

    fragPosition = vec3(pc.matModel*vec4(vertexPosition, 1.0));

    fragNormal = normalize(normalMatrix*vertexNormal);

    vec3 fragTangent = normalize(normalMatrix*vertexTangent.xyz);
    fragTangent = normalize(fragTangent - dot(fragTangent, fragNormal)*fragNormal);

    vec3 fragBinormal = normalize(normalMatrix*vertexBinormal);
    fragBinormal = cross(fragNormal, fragTangent);

    TBN = transpose(mat3(fragTangent, fragBinormal, fragNormal));

    fragColor = vertexColor;
    fragTexCoord = vertexTexCoord;

    gl_Position = pc.mvp*vec4(vertexPosition, 1.0);
}
