#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragHighlighted;
layout(location = 5) in vec4 fragBaseColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Wireframe highlight pass
    if (fragHighlighted == 1) {
        outColor = fragBaseColor;
        return;
    }

    vec4 texColor = texture(albedoMap, fragTexCoord);
    outColor = texColor * fragBaseColor;
}
