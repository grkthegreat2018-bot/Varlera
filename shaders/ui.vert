#version 450

layout(location = 0) in vec2 inPos;   /* NDC */
layout(location = 1) in vec4 inCol;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inTex;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out float vTex;

void main() {
    vCol = inCol;
    vUv = inUv;
    vTex = inTex;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
