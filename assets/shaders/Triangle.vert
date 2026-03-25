#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 lightDir;
    vec3 lightColor;
    vec3 ambientColor;
    vec3 cameraPos;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    float metallic;
    float roughness;
    int highlighted;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out int fragHighlighted;
layout(location = 3) out vec3 fragNormal;
layout(location = 4) out vec3 fragWorldPos;
layout(location = 5) out vec4 fragBaseColor;
layout(location = 6) out float fragMetallic;
layout(location = 7) out float fragRoughness;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragHighlighted = pc.highlighted;
    fragNormal = mat3(transpose(inverse(pc.model))) * inNormal;
    fragWorldPos = worldPos.xyz;
    fragBaseColor = pc.baseColor;
    fragMetallic = pc.metallic;
    fragRoughness = pc.roughness;
}
