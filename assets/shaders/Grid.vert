#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 lightDir;
    vec3 lightColor;
    vec3 ambientColor;
    vec3 cameraPos;
} ubo;

layout(location = 0) out vec3 nearPoint;
layout(location = 1) out vec3 farPoint;

// Fullscreen quad (6 vertices, 2 triangles)
vec3 positions[6] = vec3[](
    vec3(-1, -1, 0), vec3(1, -1, 0), vec3(1, 1, 0),
    vec3(-1, -1, 0), vec3(1, 1, 0), vec3(-1, 1, 0)
);

vec3 unprojectPoint(vec3 p) {
    mat4 viewProjInv = inverse(ubo.proj * ubo.view);
    vec4 unprojected = viewProjInv * vec4(p, 1.0);
    return unprojected.xyz / unprojected.w;
}

void main() {
    vec3 p = positions[gl_VertexIndex];
    nearPoint = unprojectPoint(vec3(p.xy, 0.0));
    farPoint  = unprojectPoint(vec3(p.xy, 1.0));
    gl_Position = vec4(p, 1.0);
}
