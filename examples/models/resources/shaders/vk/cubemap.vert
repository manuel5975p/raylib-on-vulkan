#version 450

// Vulkan port of resources/shaders/glsl330/cubemap.vs

layout(location = 0) in vec3 vertexPosition;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matProjection;
    mat4 matView;
};

layout(location = 0) out vec3 fragPosition;

void main()
{
    // Calculate fragment position based on model transformations
    fragPosition = vertexPosition;

    // Calculate final vertex position
    gl_Position = matProjection*matView*vec4(vertexPosition, 1.0);
}
