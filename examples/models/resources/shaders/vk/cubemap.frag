#version 450

// Vulkan port of resources/shaders/glsl330/cubemap.fs
// Equirectangular panorama is bound to texture slot 0 -> set 1, binding 0

layout(location = 0) in vec3 fragPosition;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    mat4 matProjection;
    mat4 matView;
};

layout(set = 1, binding = 0) uniform sampler2D equirectangularMap;

layout(location = 0) out vec4 finalColor;

vec2 SampleSphericalMap(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= vec2(0.1591, 0.3183);
    uv += 0.5;
    return uv;
}

void main()
{
    // Normalize local position
    vec2 uv = SampleSphericalMap(normalize(fragPosition));

    // Fetch color from texture map
    vec3 color = texture(equirectangularMap, uv).rgb;

    // Calculate final fragment color
    finalColor = vec4(color, 1.0);
}
