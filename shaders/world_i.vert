#version 450
/* Instanced path: all world blocks, selection boxes, handles. */

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
layout(location = 6) in vec3 inCol;     /* baked vertex color (VCOL models) */
layout(location = 2) in vec3 iPos;
layout(location = 3) in vec3 iScale;
layout(location = 4) in vec4 iCol;      /* RGBA8 UNORM */
layout(location = 5) in uint iFlags;    /* bit0 unlit, bits1-2 rot90, 5-8 tex, 9 vcol, 10 spin */

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;        /* w = time */
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
} u;

layout(location = 0) out vec3 vNrm;
layout(location = 1) out vec3 vWorld;
layout(location = 2) out vec4 vColor;
layout(location = 3) flat out uint vFlags;
layout(location = 4) out vec3 vLocal;   /* world-space offset from block center */

vec2 rotY(vec2 p, uint k) {
    if (k == 1u) return vec2(p.y, -p.x);
    if (k == 2u) return vec2(-p.x, -p.y);
    if (k == 3u) return vec2(-p.y, p.x);
    return p;
}

vec2 spinY(vec2 p, float a) {
    float c = cos(a), s = sin(a);
    return vec2(p.x*c - p.y*s, p.x*s + p.y*c);
}

void main() {
    vec3 lp = inPos * iScale;
    vec3 n  = inNrm;
    uint k = (iFlags >> 1) & 3u;
    lp.xz = rotY(lp.xz, k);
    n.xz  = rotY(n.xz, k);
    if ((iFlags & 1024u) != 0u) {        /* RKIF_SPIN: play-mode spin about Y */
        float a = u.camPos.w * 2.6;
        lp.xz = spinY(lp.xz, a);
        n.xz  = spinY(n.xz, a);
    }
    vLocal = lp;                         /* post-scale local pos for tex patterns */
    vec4 w = vec4(iPos + lp, 1.0);
    vWorld = w.xyz;
    vNrm = n;
    vColor = (iFlags & 512u) != 0u ? vec4(inCol, iCol.a) : iCol;
    vFlags = iFlags;
    gl_Position = u.viewProj * w;
}
