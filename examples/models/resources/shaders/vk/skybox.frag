#version 450

// Vulkan port of resources/shaders/glsl330/skybox.fs
// rlvk only exposes 4 sampler slots (set 1, binding 0..3) while MATERIAL_MAP_CUBEMAP is 7,
// so the example puts the cubemap in MATERIAL_MAP_DIFFUSE -> set 1, binding 0

layout(location = 0) in vec3 fragPosition;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matProjection;
    mat4 matView;
    int vflipped;
    int doGamma;
};

layout(set = 1, binding = 0) uniform samplerCube environmentMap;

layout(location = 0) out vec4 finalColor;

void main()
{
    // Fetch color from texture map
    vec3 color = vec3(0.0);

    if (vflipped != 0) color = texture(environmentMap, vec3(fragPosition.x, -fragPosition.y, fragPosition.z)).rgb;
    else color = texture(environmentMap, fragPosition).rgb;

    if (doGamma != 0) // Apply gamma correction
    {
        color = color/(color + vec3(1.0));
        color = pow(color, vec3(1.0/2.2));
    }

    // Calculate final fragment color
    finalColor = vec4(color, 1.0);
}
