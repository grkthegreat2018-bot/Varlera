#version 450
/* Bloom upsample: 4-tap tent (bilinear at the 2x2 offsets), written with
   additive blending so each mip accumulates over the downsampled base. */

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec2 texel; float knee; float pad; } pc;

void main() {
    vec3 c = texture(src, vUv + pc.texel * vec2(-1.0, -1.0)).rgb
           + texture(src, vUv + pc.texel * vec2( 1.0, -1.0)).rgb
           + texture(src, vUv + pc.texel * vec2(-1.0,  1.0)).rgb
           + texture(src, vUv + pc.texel * vec2( 1.0,  1.0)).rgb;
    outColor = vec4(c * 0.25, 1.0);
}
