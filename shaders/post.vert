#version 450
/* Fullscreen triangle for the post chain; emits uv for the frag stages. */

layout(location = 0) out vec2 vUv;

void main() {
    vec2 p = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0,
                  gl_VertexIndex == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
