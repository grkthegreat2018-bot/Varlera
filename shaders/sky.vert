#version 450
/* Fullscreen-triangle sky: unprojects each pixel to a world-space view
   direction; the fragment stage shades the gradient + sun disc. */

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
    vec4 texRect[64];
    float texFps[64];
    vec4 groundCol;
} u;

layout(location = 0) out vec3 vDir;

void main() {
    /* covers the screen with one oversized triangle */
    vec2 pos = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0,
                    gl_VertexIndex == 2 ? 3.0 : -1.0);
    /* far-plane world position (reversed-Z far = 0) minus eye = view dir */
    vec4 p = inverse(u.viewProj) * vec4(pos, 0.0, 1.0);
    vDir = p.xyz / p.w - u.camPos.xyz;
    gl_Position = vec4(pos, 1.0, 1.0);
}
