#version 450
/* Bloom downsample: 4-tap box (bilinear at the 2x2 block centers).
   First level applies a soft-knee brightness threshold so only >1 HDR
   emitters (glow materials, sun disc) feed the chain. */

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec2 texel; float knee; float pad; } pc;

void main() {
    vec3 c = texture(src, vUv + pc.texel * vec2(-1.0, -1.0)).rgb
           + texture(src, vUv + pc.texel * vec2( 1.0, -1.0)).rgb
           + texture(src, vUv + pc.texel * vec2(-1.0,  1.0)).rgb
           + texture(src, vUv + pc.texel * vec2( 1.0,  1.0)).rgb;
    c *= 0.25;
    if (pc.knee > 0.0) {
        const float T = 1.0;                     /* brightness threshold */
        float l = max(c.r, max(c.g, c.b));
        float soft = clamp(l - T + pc.knee, 0.0, 2.0 * pc.knee);
        soft = soft * soft / (4.0 * pc.knee);
        c *= max(soft, l - T) / max(l, 1e-4);
    }
    outColor = vec4(c, 1.0);
}
