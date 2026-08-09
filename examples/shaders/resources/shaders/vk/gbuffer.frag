#version 450

// Port of glsl330/gbuffer.fs to the rayvulkan shader ABI (MRT: 3 color attachments)

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;
layout(set = 1, binding = 1) uniform sampler2D specularTexture;

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;

void main()
{
    // Store the fragment position vector in the first gbuffer texture
    gPosition = vec4(fragPosition, 1.0);
    // Also store the per-fragment normals into the gbuffer
    gNormal = vec4(normalize(fragNormal), 1.0);
    // And the diffuse per-fragment color, specular intensity in the alpha component
    gAlbedoSpec.rgb = texture(diffuseTexture, fragTexCoord).rgb;
    gAlbedoSpec.a = texture(specularTexture, fragTexCoord).r;
}
