#version 450

// Port of glsl330/eratosthenes.fs to the rayvulkan shader ABI
// The Sieve of Eratosthenes -- a simple shader by ProfJski

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 finalColor;

// Make a nice spectrum of colors based on counter and maxSize
vec4 Colorizer(float counter, float maxSize)
{
    float normsize = counter/maxSize;

    float red = smoothstep(0.3, 0.7, normsize);
    float green = sin(3.14159*normsize);
    float blue = 1.0 - smoothstep(0.0, 0.4, normsize);

    return vec4(0.8*red, 0.8*green, 0.8*blue, 1.0);
}

void main()
{
    vec4 color = vec4(1.0);
    float scale = 1000.0; // Makes 1000x1000 square grid
    int value = int(scale*floor(fragTexCoord.y*scale) + floor(fragTexCoord.x*scale));

    if ((value == 0) || (value == 1) || (value == 2)) finalColor = vec4(1.0);
    else
    {
        for (int i = 2; (float(i) < max(2.0, sqrt(float(value)) + 1.0)); i++)
        {
            if ((float(value) - float(i)*floor(float(value)/float(i))) == 0.0)
            {
                color = Colorizer(float(i), scale);
            }
        }

        finalColor = color;
    }
}
