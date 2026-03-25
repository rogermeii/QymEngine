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

const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz normal distribution
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry function (single direction)
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's geometry function (both view and light directions)
float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Wireframe pass: output solid color, skip lighting
    if (fragHighlighted == 1) {
        outColor = fragBaseColor;
        return;
    }

    // Normal mapping via screen-space derivatives
    vec3 N = normalize(fragNormal);
    vec3 mapN = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;
    vec3 Q1 = dFdx(fragWorldPos);
    vec3 Q2 = dFdy(fragWorldPos);
    vec2 st1 = dFdx(fragTexCoord);
    vec2 st2 = dFdy(fragTexCoord);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);
    N = normalize(TBN * mapN);

    vec3 albedo = texture(texSampler, fragTexCoord).rgb * fragBaseColor.rgb;
    float metallic = fragMetallic;
    float roughness = max(fragRoughness, 0.04); // clamp to avoid divide-by-zero

    vec3 V = normalize(ubo.cameraPos - fragWorldPos);
    vec3 L = normalize(-ubo.lightDir);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001); // avoid zero
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // F0: dielectric base reflectance 0.04, metallic uses albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular BRDF
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3  F = fresnelSchlick(HdotV, F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    // Energy conservation: diffuse uses remaining energy after specular
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    // Direct lighting
    vec3 Lo = (diffuse + specular) * ubo.lightColor * NdotL;

    // Ambient (fixed, no IBL)
    vec3 ambient = ubo.ambientColor * albedo;

    vec3 result = ambient + Lo;

    outColor = vec4(result, fragBaseColor.a);
}
