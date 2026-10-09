#version 450
/* Depth-only instanced vertex shader for the shadow map. */

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;   /* unused */
layout(location = 2) in vec3 iPos;
layout(location = 3) in vec3 iScale;
layout(location = 4) in vec4 iCol;    /* unused */
layout(location = 5) in uint iFlags;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
} u;

vec2 rotY(vec2 p, uint k) {
    if (k == 1u) return vec2(p.y, -p.x);
    if (k == 2u) return vec2(-p.x, -p.y);
    if (k == 3u) return vec2(-p.y, p.x);
    return p;
}

void main() {
    vec3 lp = inPos * iScale;
    lp.xz = rotY(lp.xz, (iFlags >> 1) & 3u);
    gl_Position = u.shadowVP * vec4(iPos + lp, 1.0);
}
