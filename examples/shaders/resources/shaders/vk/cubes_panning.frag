#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    float uTime;
} ubo;

layout(location = 0) out vec4 finalColor;

#define PI 3.14159265358979323846

const float divisions = 5.0;

vec2 VectorRotateTime(vec2 v, float speed)
{
    float time = ubo.uTime*speed;
    float localTime = fract(time);  // The time domain this works on is 1 sec
    float angle = 0.0;

    if ((localTime >= 0.0) && (localTime < 0.25)) angle = 0.0;
    else if ((localTime >= 0.25) && (localTime < 0.50)) angle = PI/4.0*sin(2.0*PI*localTime - PI/2.0);
    else if ((localTime >= 0.50) && (localTime < 0.75)) angle = PI*0.25;
    else if ((localTime >= 0.75) && (localTime < 1.00)) angle = PI/4.0*sin(2.0*PI*localTime);

    // Rotate vector by angle
    v -= 0.5;
    v = mat2(cos(angle), -sin(angle), sin(angle), cos(angle))*v;
    v += 0.5;

    return v;
}

float Rectangle(in vec2 st, in float size, in float fill)
{
    float roundSize = 0.5 - size/2.0;
    float left = step(roundSize, st.x);
    float top = step(roundSize, st.y);
    float bottom = step(roundSize, 1.0 - st.y);
    float right = step(roundSize, 1.0 - st.x);

    return (left*bottom*right*top)*fill;
}

void main()
{
    vec2 fragPos = fragTexCoord;
    fragPos.xy += ubo.uTime/9.0;

    fragPos *= divisions;
    vec2 ipos = floor(fragPos);  // Get the integer coords
    vec2 fpos = fract(fragPos);  // Get the fractional coords

    fpos = VectorRotateTime(fpos, 0.2);

    float alpha = Rectangle(fpos, 0.216, 1.0);
    vec3 color = vec3(0.3, 0.3, 0.3);

    finalColor = vec4(color, alpha);
}
