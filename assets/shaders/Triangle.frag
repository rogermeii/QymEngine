#version 450

layout(binding = 0) uniform UniformBufferObject {
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

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 texColor = texture(texSampler, fragTexCoord).rgb;
    vec3 normal = normalize(fragNormal);
    vec3 lightD = normalize(-ubo.lightDir);

    // Ambient
    vec3 ambient = ubo.ambientColor;

    // Diffuse
    float diff = max(dot(normal, lightD), 0.0);
    vec3 diffuse = diff * ubo.lightColor;

    // Specular (Blinn-Phong)
    vec3 viewDir = normalize(ubo.cameraPos - fragWorldPos);
    vec3 halfDir = normalize(lightD + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 32.0);
    vec3 specular = spec * ubo.lightColor * 0.5;

    vec3 result = (ambient + diffuse + specular) * texColor;
    outColor = vec4(result, 1.0);

    if (fragHighlighted == 1) {
        outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);
    }
}
