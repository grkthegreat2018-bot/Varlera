#version 450

layout(location = 0) in vec3 vNrm;
layout(location = 1) in vec3 vWorld;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vFlags;
layout(location = 4) in vec3 vLocal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
} u;

layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;

float shadowFactor(vec3 wp) {
    vec4 lp = u.shadowVP * vec4(wp, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float ref = ndc.z - 0.0025;                    /* depth bias */
    if (ref <= 0.0 || ref >= 1.0) return 1.0;
    /* 4-tap PCF on top of hardware-compare linear filter */
    vec2 ts = vec2(textureSize(shadowMap, 0));
    vec2 px = 1.0 / ts;
    float s = 0.0;
    s += texture(shadowMap, vec3(uv + px * vec2(-0.5, -0.5), ref));
    s += texture(shadowMap, vec3(uv + px * vec2( 0.5, -0.5), ref));
    s += texture(shadowMap, vec3(uv + px * vec2(-0.5,  0.5), ref));
    s += texture(shadowMap, vec3(uv + px * vec2( 0.5,  0.5), ref));
    return s * 0.25;
}

/* procedural block textures (id in vFlags bits 5-8):
   1 studs (Blocksworld lego bumps), 2 checker, 3 stripes, 4 dots, 5 fine grid */
void applyTex(inout vec3 base, inout vec3 n, uint tex) {
    if (tex == 0u) return;
    vec2 pp = abs(n.y) > 0.6 ? vLocal.xz : (abs(n.x) > 0.6 ? vLocal.zy : vLocal.xy);
    if (tex == 1u) {
        if (n.y > 0.5f) {           /* studs only on top faces */
            vec2 q = fract(vLocal.xz / 0.4) - 0.5;
            float r = length(q);
            float bump = smoothstep(0.20, 0.10, r);
            n = normalize(n + vec3(q.x, 0.0, q.y) * bump * 1.6);
            base *= 1.0 - 0.18 * smoothstep(0.24, 0.20, r) * (1.0 - bump);
        }
    } else if (tex == 2u) {
        int c = int(floor(vLocal.x * 2.0) + floor(vLocal.y * 2.0) + floor(vLocal.z * 2.0));
        if ((c & 1) != 0) base *= 0.78;
    } else if (tex == 3u) {
        if (fract(pp.x * 1.6) > 0.5) base *= 0.80;
    } else if (tex == 4u) {
        if (length(fract(pp * 2.5) - 0.5) < 0.16) base *= 0.72;
    } else if (tex == 5u) {
        vec2 g = abs(fract(pp * 4.0) - 0.5);
        if (max(g.x, g.y) > 0.44) base *= 0.82;
    }
}

void main() {
    if ((vFlags & 1u) != 0u) { outColor = vColor; return; }
    vec3 n = normalize(vNrm);
    vec3 base = vColor.rgb;
    applyTex(base, n, (vFlags >> 5) & 15u);
    vec3 L = normalize(u.lightDir.xyz);
    float diff = max(dot(n, L), 0.0);
    float hemi = 0.32 + 0.22 * (n.y * 0.5 + 0.5);  /* sky-ish ambient */
    float sh = shadowFactor(vWorld);
    vec3 col = base * (hemi + 0.95 * diff * sh);
    if ((vFlags & 8u) == 0u) {                     /* fog unless NOFOG */
        float d = distance(u.camPos.xyz, vWorld);
        float f = clamp((d - u.fogParams.x) / (u.fogParams.y - u.fogParams.x), 0.0, 1.0);
        col = mix(col, u.fogColor.rgb, f);
    }
    outColor = vec4(col, vColor.a);
}
