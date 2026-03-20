#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragHighlighted;

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(texSampler, fragTexCoord);
    if (fragHighlighted == 1) {
        outColor = mix(outColor, vec4(1.0, 0.5, 0.0, 1.0), 0.3);
    }
}
