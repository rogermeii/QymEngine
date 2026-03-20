#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 lightDir;
    vec3 lightColor;
    vec3 ambientColor;
    vec3 cameraPos;
} ubo;

layout(location = 0) in vec3 nearPoint;
layout(location = 1) in vec3 farPoint;

layout(location = 0) out vec4 outColor;

vec4 grid(vec3 fragPos, float scale, vec4 color) {
    vec2 coord = fragPos.xz / scale;
    vec2 derivative = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(g.x, g.y);
    float alpha = 1.0 - min(line, 1.0);
    return vec4(color.rgb, alpha * color.a);
}

void main() {
    // Ray-plane intersection with y=0
    float t = -nearPoint.y / (farPoint.y - nearPoint.y);

    if (t < 0.0 || t > 1.0) {
        // Sky gradient based on view direction
        vec3 dir = normalize(farPoint - nearPoint);
        float h = max(dir.y, 0.0);
        vec3 skyBottom = vec3(0.4, 0.4, 0.45);
        vec3 skyTop = vec3(0.15, 0.15, 0.3);
        outColor = vec4(mix(skyBottom, skyTop, h), 1.0);
        gl_FragDepth = 1.0;
        return;
    }

    vec3 fragPos = nearPoint + t * (farPoint - nearPoint);

    // Distance fade
    float dist = length(fragPos.xz - ubo.cameraPos.xz);
    float fade = 1.0 - smoothstep(20.0, 50.0, dist);

    // Small grid (0.5m spacing)
    vec4 smallGrid = grid(fragPos, 0.5, vec4(0.4, 0.4, 0.4, 0.4));

    // Large grid (5m spacing)
    vec4 largeGrid = grid(fragPos, 5.0, vec4(0.5, 0.5, 0.5, 0.7));

    // X axis (red line at z=0)
    float xAxis = 1.0 - min(abs(fragPos.z) / fwidth(fragPos.z), 1.0);
    // Z axis (blue line at x=0)
    float zAxis = 1.0 - min(abs(fragPos.x) / fwidth(fragPos.x), 1.0);

    vec4 color = smallGrid;
    color = mix(color, largeGrid, largeGrid.a);

    // Color axis lines
    if (xAxis > 0.5) color = vec4(1.0, 0.2, 0.2, 1.0);
    if (zAxis > 0.5) color = vec4(0.2, 0.2, 1.0, 1.0);

    color.a *= fade;

    if (color.a < 0.01) discard;

    outColor = color;

    // Write depth for grid
    vec4 clipPos = ubo.proj * ubo.view * vec4(fragPos, 1.0);
    float ndcDepth = clipPos.z / clipPos.w;
    gl_FragDepth = ndcDepth;
}
