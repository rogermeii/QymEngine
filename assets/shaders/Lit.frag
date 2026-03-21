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
layout(set = 1, binding = 1) uniform sampler2D normalMap;

layout(location = 0) out vec4 outColor;

void main() {
    // Wireframe pass: output solid color, skip lighting
    if (fragHighlighted == 1) {
        outColor = fragBaseColor;
        return;
    }

    // Sample and apply normal map
    vec3 normal = normalize(fragNormal);
    vec3 mapN = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;
    vec3 Q1 = dFdx(fragWorldPos);
    vec3 Q2 = dFdy(fragWorldPos);
    vec2 st1 = dFdx(fragTexCoord);
    vec2 st2 = dFdy(fragTexCoord);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = normalize(cross(normal, T));
    mat3 TBN = mat3(T, B, normal);
    normal = normalize(TBN * mapN);

    vec3 albedo = texture(texSampler, fragTexCoord).rgb * fragBaseColor.rgb;
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
}
