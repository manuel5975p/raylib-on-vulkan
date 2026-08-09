#version 450

// Port of glsl330/outline_hull.vs to the rayvulkan shader ABI

layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(push_constant) uniform PC {
    mat4 mvp;
    mat4 matModel;
} pc;

layout(std140, set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    float outlineThickness;
} ubo;

void main() {
    // Extrude vertex along its normal to create the hull
    vec3 extruded = vertexPosition + vertexNormal*ubo.outlineThickness;
    gl_Position = pc.mvp*vec4(extruded, 1.0);
}
