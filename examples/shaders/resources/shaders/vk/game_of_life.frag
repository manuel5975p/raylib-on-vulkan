#version 450

// Port of glsl330/game_of_life.fs to the rayvulkan shader ABI

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec2 resolution;    // Input size in pixels of the textures
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

void main()
{
    // Size of one pixel in texture coordinates (from 0.0 to 1.0)
    float x = 1.0/ubo.resolution.x;
    float y = 1.0/ubo.resolution.y;

    // Status of the current cell (1 = alive, 0 = dead)
    int origValue = (texture(texture0, fragTexCoord).r < 0.1)? 1 : 0;

    // Sum of alive neighbors
    int sumValue = (texture(texture0, vec2(fragTexCoord.x - x, fragTexCoord.y - y)).r < 0.1)? 1 : 0;
    sumValue    += (texture(texture0, vec2(fragTexCoord.x - x, fragTexCoord.y    )).r < 0.1)? 1 : 0;
    sumValue    += (texture(texture0, vec2(fragTexCoord.x - x, fragTexCoord.y + y)).r < 0.1)? 1 : 0;

    sumValue    += (texture(texture0, vec2(fragTexCoord.x,     fragTexCoord.y - y)).r < 0.1)? 1 : 0;
    sumValue    += (texture(texture0, vec2(fragTexCoord.x,     fragTexCoord.y + y)).r < 0.1)? 1 : 0;

    sumValue    += (texture(texture0, vec2(fragTexCoord.x + x, fragTexCoord.y - y)).r < 0.1)? 1 : 0;
    sumValue    += (texture(texture0, vec2(fragTexCoord.x + x, fragTexCoord.y    )).r < 0.1)? 1 : 0;
    sumValue    += (texture(texture0, vec2(fragTexCoord.x + x, fragTexCoord.y + y)).r < 0.1)? 1 : 0;

    // Game of life rules
    if (((origValue == 1) && (sumValue == 2)) || sumValue == 3)
        finalColor = vec4(0.0, 0.0, 0.0, 1.0);      // Alive: draw the pixel black
    else
        finalColor = fragColor;                     // Dead: draw the pixel with the background color
}
