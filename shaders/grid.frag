#version 450

layout(location = 0) in vec3 vNrm;
layout(location = 1) in vec3 vWorld;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vFlags;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
    vec4 texRect[64];      /* unused here - keeps groundCol offset aligned */
    float texFps[64];      /* unused - keeps layout aligned */
    vec4 groundCol;
} u;

layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;

float gridLine(vec2 p, float spacing) {
    vec2 q = abs(fract(p / spacing - 0.5) - 0.5) * spacing;
    vec2 w = fwidth(p);
    vec2 g = 1.0 - smoothstep(vec2(0.0), w * 1.5, q);
    return max(g.x, g.y);
}

float shadowFactor(vec3 wp) {
    vec4 lp = u.shadowVP * vec4(wp, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float ref = ndc.z - 0.0025;
    if (ref <= 0.0 || ref >= 1.0) return 1.0;
    return texture(shadowMap, vec3(uv, ref));
}

void main() {
    int style = int(u.groundCol.a + 0.5);
    vec3 base = u.groundCol.rgb;
    vec3 col = base;
    if (style == 0) {                        /* GRID: minor 1u + major 8u lines */
        float line = max(gridLine(vWorld.xz, 1.0) * 0.18,
                         gridLine(vWorld.xz, 8.0) * 0.38);
        col = mix(base, vec3(0.36, 0.38, 0.44), line);
    } else if (style == 1) {                 /* CHECK: 4u checkerboard + edge lines */
        float ck = mod(floor(vWorld.x / 4.0) + floor(vWorld.z / 4.0), 2.0);
        col = base * mix(1.0, 0.80, ck);
        col = mix(col, vec3(0.36, 0.38, 0.44), gridLine(vWorld.xz, 4.0) * 0.30);
    } else if (style == 2) {                 /* DOTS: dot per 2u cell */
        float d = length(fract(vWorld.xz / 2.0) - 0.5) * 2.0;
        float w = fwidth(d) * 1.5;
        col = mix(base, base * 0.72, (1.0 - smoothstep(0.16 - w, 0.16 + w, d)) * 0.5);
    }                                        /* style 3 PLAIN: flat base */
    /* skip shadow lookup when viewing the underside (shadows would bleed through) */
    float sh = u.camPos.y > vWorld.y ? shadowFactor(vWorld) : 1.0;
    col *= 0.55 + 0.45 * sh;
    if (u.camPos.y < vWorld.y) col *= 0.8;   /* dim underside slightly */
    float d = distance(u.camPos.xyz, vWorld);
    float f = clamp((d - u.fogParams.x) / (u.fogParams.y - u.fogParams.x), 0.0, 1.0);
    outColor = vec4(mix(col, u.fogColor.rgb, f), 1.0);
}
