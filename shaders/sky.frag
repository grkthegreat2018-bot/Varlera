#version 450
/* Stylized sky: fog color at the horizon grading toward a deeper zenith,
   ground tint below the horizon, warm sun disc + glow along lightDir. */

layout(location = 0) in vec3 vDir;
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
    vec4 groundCol;
} u;

void main() {
    vec3 d = normalize(vDir);
    vec3 sky = u.fogColor.rgb;

    /* zenith: pull the sky tone down into a deeper blue */
    vec3 zen = sky * vec3(0.42, 0.55, 0.95) + vec3(0.01, 0.02, 0.10);
    float h = pow(clamp(d.y, 0.0, 1.0), 0.55);
    vec3 col = mix(sky, zen, h);

    /* below horizon: settle into a dim ground tone */
    col = mix(u.groundCol.rgb * 0.72, col, smoothstep(-0.10, 0.03, d.y));

    /* sun: tight disc + wide warm halo */
    float s = max(dot(d, normalize(u.lightDir.xyz)), 0.0);
    col += vec3(1.0, 0.90, 0.70) * (pow(s, 900.0) * 1.4 + pow(s, 20.0) * 0.10);

    outColor = vec4(col, 1.0);
}
