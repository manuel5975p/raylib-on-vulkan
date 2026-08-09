#version 450

// Port of glsl330/deferred_shading.vs to the rayvulkan shader ABI
// NOTE: Positions are already in clip space (fullscreen quad), mvp is ignored

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;

layout(location = 0) out vec2 texCoord;

void main()
{
    gl_Position = vec4(vertexPosition, 1.0);
    texCoord = vertexTexCoord;
}
