#version 450

layout(location = 0) in vec3 vNrm;
layout(location = 1) in vec3 vWorld;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vFlags;
layout(location = 4) in vec3 vLocal;
layout(location = 5) in vec3 vScale;
layout(location = 6) flat in float vSeed;
layout(location = 7) flat in float vTex;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 iconVP;
    mat4 shadowVP;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 fogParams;
    vec4 texRect[64];
} u;

layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 1, binding = 1) uniform sampler2D texAtlas;

const float TUV = 128.0 / 1024.0;   /* one atlas tile in uv units */

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

/* atlas textures: box-projected face UVs.
   mode 0 stretch (whole face), 1 loop (per unit), 2 proc (random sub-tile per unit).
   type: bit13 set = override (replace color, alpha cutout), clear = overlay (multiply). */
uint texhash(uvec3 c, float seed) {
    uint h = c.x * 1973u + c.y * 9277u + c.z * 26699u + uint(seed * 7919.0);
    h ^= h >> 13u; h *= 2654435761u; h ^= h >> 16u;
    return h;
}

vec4 sampleTex(vec3 n, uint tid, uint mode, out float cov) {
    cov = 0.0;
    vec4 tr = u.texRect[tid];
    vec3 an = abs(n);
    vec2 pp; vec2 fs;
    if (an.y >= an.x && an.y >= an.z)      { pp = vLocal.xz; fs = vScale.xz; }
    else if (an.x >= an.z)                 { pp = vLocal.zy; fs = vScale.zy; }
    else                                   { pp = vLocal.xy; fs = vScale.xy; }
    vec2 tuv;                             /* inside texture rect, tile units */
    if (mode == 0u) {
        tuv = (pp / fs + 0.5) * tr.zw;    /* stretch whole sheet across face */
    } else if (mode == 1u) {
        tuv = mod(pp + 0.5, tr.zw);       /* loop whole sheet every tileW/H units */
    } else {
        vec2 cell = floor(pp + 0.5);
        uint h = texhash(uvec3(uvec2(cell + 1024.0), uint(tid)), vSeed);
        int sw = int(tr.z), sh = int(tr.w);
        vec2 st = vec2(h % uint(sw), (h / uint(sw)) % uint(sh));
        tuv = st + fract(pp + 0.5);       /* random sub-tile per unit cell */
    }
    cov = 1.0;
    return texture(texAtlas, tr.xy + tuv * TUV);
}

void main() {
    if ((vFlags & 1u) != 0u) { outColor = vColor; return; }
    vec3 n = normalize(vNrm);
    vec3 base = vColor.rgb;
    uint tid = (vFlags >> 5) & 63u;
    if (vTex >= 0.0) tid = uint(vTex + 0.5);   /* per-vertex material layer */
    if (tid != 0u) {
        float cov;
        vec4 t = sampleTex(n, tid, (vFlags >> 11) & 3u, cov);
        if ((vFlags & 8192u) != 0u) {      /* override: exact texture */
            if (t.a < 0.5) discard;
            base = t.rgb;
        } else {                           /* overlay: tint keeps block color */
            base *= mix(vec3(1.0), t.rgb * 2.0, t.a);
        }
    }
    vec3 L = normalize(u.lightDir.xyz);
    float sh = shadowFactor(vWorld);
    if ((vFlags & 16u) != 0u) {         /* icon: fixed key light, no shadow */
        L = normalize(vec3(-0.40, 0.85, 0.50));
        sh = 1.0;
    }
    float diff = max(dot(n, L), 0.0);
    float hemi = 0.32 + 0.22 * (n.y * 0.5 + 0.5);  /* sky-ish ambient */
    vec3 col = base * (hemi + 0.95 * diff * sh);
    /* material: bits 17-18 -> 1 glossy, 2 metal, 3 emissive (glow feeds bloom) */
    uint mat = (vFlags >> 17) & 3u;
    if (mat == 1u || mat == 2u) {
        vec3 V = normalize(u.camPos.xyz - vWorld);
        vec3 H = normalize(L + V);
        float sp = pow(max(dot(n, H), 0.0), 32.0) * sh;
        col += (mat == 1u ? vec3(0.55) : base * 0.9) * sp;
    } else if (mat == 3u) {
        col = base * 1.55 + col * 0.2;           /* glow: self-bright, bloom src */
    }
    if ((vFlags & 8u) == 0u) {                     /* fog unless NOFOG */
        float d = distance(u.camPos.xyz, vWorld);
        float f = clamp((d - u.fogParams.x) / (u.fogParams.y - u.fogParams.x), 0.0, 1.0);
        col = mix(col, u.fogColor.rgb, f);
    }
    outColor = vec4(col, vColor.a);
}
