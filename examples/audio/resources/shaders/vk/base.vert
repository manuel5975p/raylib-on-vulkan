#version 450

// Copy of the rayvulkan default batch vertex shader (src/shaders/default.vert).
// Needed because LoadShader(NULL, fs) does not fall back to the built-in vertex
// shader on the Vulkan backend; examples with an FS-only shader load this.

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
}
