#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 lightDir;
    vec3 lightColor;
    vec3 ambientColor;
    vec3 cameraPos;
} ubo;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragHighlighted;
layout(location = 3) in vec3 fragNormal;
layout(location = 4) in vec3 fragWorldPos;
layout(location = 5) in vec4 fragBaseColor;
layout(location = 6) in float fragMetallic;
layout(location = 7) in float fragRoughness;

layout(set = 1, binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 texColor = texture(texSampler, fragTexCoord).rgb;
    vec3 albedo = texColor * fragBaseColor.rgb;
    vec3 normal = normalize(fragNormal);
    vec3 lightD = normalize(-ubo.lightDir);

    // Ambient
    vec3 ambient = ubo.ambientColor * albedo;

    // Diffuse (reduced by metallic)
    float diff = max(dot(normal, lightD), 0.0);
    vec3 diffuse = diff * ubo.lightColor * albedo * (1.0 - fragMetallic);

    // Specular (Blinn-Phong, shininess controlled by roughness)
    vec3 viewDir = normalize(ubo.cameraPos - fragWorldPos);
    vec3 halfDir = normalize(lightD + viewDir);
    float shininess = mix(256.0, 8.0, fragRoughness);
    float spec = pow(max(dot(normal, halfDir), 0.0), shininess);
    float specStrength = mix(0.3, 1.0, fragMetallic);
    vec3 specular = spec * ubo.lightColor * specStrength;

    vec3 result = ambient + diffuse + specular;
    outColor = vec4(result, fragBaseColor.a);

    if (fragHighlighted == 1) {
        outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);
    }
}
