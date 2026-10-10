#version 450
/* Non-instanced path: grid, icons, anything driven by push constants. */

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
layout(location = 6) in vec3 inCol;
layout(location = 7) in float inTex;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;   /* x: start, y: end */
    vec4 texRect[64]; /* atlas rect per texture id: u0,v0,tileW,tileH */
} u;

layout(push_constant) uniform Push {
    mat4  model;
    vec4  color;
    uint  flags;      /* bit0 unlit, bit3 no-fog, bit4 iconVP */
} p;

layout(location = 0) out vec3 vNrm;
layout(location = 1) out vec3 vWorld;
layout(location = 2) out vec4 vColor;
layout(location = 3) flat out uint vFlags;
layout(location = 4) out vec3 vLocal;
layout(location = 5) out vec3 vScale;
layout(location = 6) flat out float vSeed;
layout(location = 7) flat out float vTex;

void main() {
    vec4 w = p.model * vec4(inPos, 1.0);
    vWorld = w.xyz;
    vNrm = mat3(p.model) * inNrm;
    vColor = (p.flags & 16384u) != 0u ? vec4(inCol, p.color.a) : p.color;
    vFlags = p.flags;
    vLocal = w.xyz - p.model[3].xyz;
    vScale = vec3(length(p.model[0].xyz), length(p.model[1].xyz), length(p.model[2].xyz));
    vSeed = 0.0;
    vTex = inTex;
    mat4 vp = (p.flags & 16u) != 0u ? u.iconVP : u.viewProj;
    gl_Position = vp * w;
}
