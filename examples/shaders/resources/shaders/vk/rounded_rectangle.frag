// Note: SDF by Iñigo Quilez is licensed under MIT License
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    vec4 colDiffuse;
    vec4 rectangle;         // Rectangle dimensions (x, y, width, height)
    vec4 radius;            // Corner radius (top-left, top-right, bottom-left, bottom-right)
    vec4 color;
    // Shadow parameters
    vec4 shadowColor;
    vec2 shadowOffset;
    float shadowRadius;
    float shadowScale;
    // Border parameters
    vec4 borderColor;
    float borderThickness;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D texture0;

layout(location = 0) out vec4 finalColor;

// Create a rounded rectangle using signed distance field
// Thanks to Iñigo Quilez (https://www.iquilezles.org/www/articles/distfunctions/distfunctions.htm)
// And thanks to inobelar (https://www.shadertoy.com/view/fsdyzB) for shader
float RoundedRectangleSDF(vec2 fragCoord, vec2 center, vec2 halfSize, vec4 radius)
{
    vec2 fragFromCenter = fragCoord - center;

    // Determine which corner radius to use
    // NOTE: gl_FragCoord is top-left origin in Vulkan, so the y test is flipped
    radius.xy = (fragFromCenter.y < 0.0)? radius.xy : radius.zw;
    radius.x  = (fragFromCenter.x < 0.0)? radius.x  : radius.y;

    vec2 dist = abs(fragFromCenter) - halfSize + radius.x;
    return min(max(dist.x, dist.y), 0.0) + length(max(dist, 0.0)) - radius.x;
}

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);

    // Requires fragment coordinate in pixels
    vec2 fragCoord = gl_FragCoord.xy;

    vec2 halfSize = ubo.rectangle.zw*0.5;
    vec2 center = ubo.rectangle.xy + halfSize;
    float recSDF = RoundedRectangleSDF(fragCoord, center, halfSize, ubo.radius);

    vec2 shadowHalfSize = halfSize*ubo.shadowScale;
    vec2 shadowCenter = center + ubo.shadowOffset;
    float shadowSDF = RoundedRectangleSDF(fragCoord, shadowCenter, shadowHalfSize, ubo.radius);

    float recFactor = smoothstep(1.0, 0.0, recSDF);
    float shadowFactor = smoothstep(ubo.shadowRadius, 0.0, shadowSDF);
    float borderFactor = smoothstep(0.0, 1.0, recSDF + ubo.borderThickness)*recFactor;

    vec4 recColor = vec4(ubo.color.rgb, ubo.color.a*recFactor);
    vec4 shadowCol = vec4(ubo.shadowColor.rgb, ubo.shadowColor.a*shadowFactor);
    vec4 borderCol = vec4(ubo.borderColor.rgb, ubo.borderColor.a*borderFactor);

    finalColor = mix(mix(shadowCol, recColor, recColor.a), borderCol, borderCol.a);
}
