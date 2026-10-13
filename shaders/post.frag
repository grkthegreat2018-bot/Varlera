#version 450
/* Composite: HDR scene + bloom -> exposure -> ACES tonemap -> sRGB out.
   pc.flags bit0 = apply tonemap (0 = passthrough LDR path),
            bit1 = manual gamma encode (unorm surface, no hw sRGB). */

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(push_constant) uniform PC { vec2 texel; float strength; float flags; } pc;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

void main() {
    vec3 c = texture(hdr, vUv).rgb;
    c += texture(bloom, vUv).rgb * pc.strength;
    uint fl = uint(pc.flags);
    if ((fl & 1u) != 0u)
        c = aces(c * 1.30);                 /* exposure keeps mids lifted */
    if ((fl & 2u) != 0u)
        c = pow(c, vec3(1.0 / 2.2));
    outColor = vec4(c, 1.0);
}
