#version 450

// Port of glsl330/outline_hull.fs to the rayvulkan shader ABI

layout(location = 0) out vec4 finalColor;

void main() {
    finalColor = vec4(0.05, 0.05, 0.05, 1.0);
}
