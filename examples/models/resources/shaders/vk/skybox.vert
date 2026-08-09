#version 450

// Vulkan port of resources/shaders/glsl330/skybox.vs

layout(location = 0) in vec3 vertexPosition;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matProjection;
    mat4 matView;
    int vflipped;
    int doGamma;
};

layout(location = 0) out vec3 fragPosition;

void main()
{
    // Calculate fragment position based on model transformations
    fragPosition = vertexPosition;

    // Remove translation from the view matrix
    mat4 rotView = mat4(mat3(matView));
    vec4 clipPos = matProjection*rotView*vec4(vertexPosition, 1.0);

    // Calculate final vertex position
    gl_Position = clipPos;
}
