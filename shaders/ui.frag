#version 450

layout(set = 1, binding = 1) uniform sampler2D texAtlas;

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in float vTex;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = (vTex > -0.5) ? texture(texAtlas, vUv) * vCol : vCol;
    gl_FragDepth = 0.0;   /* reversed-Z: push to far so icons/overlays draw above */
}
