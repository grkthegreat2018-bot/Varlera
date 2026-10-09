/* Minimal vec3/mat4 math for Varlera. m4 is column-major (GLSL order). */
#ifndef MATH3D_H
#define MATH3D_H

#include <math.h>
#include <string.h>

typedef union { struct { float x, y, z; }; float e[3]; } v3;

static inline v3 v3_make(float x, float y, float z) { v3 v = {x, y, z}; return v; }
static inline v3 v3_add(v3 a, v3 b) { return v3_make(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline v3 v3_sub(v3 a, v3 b) { return v3_make(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline v3 v3_scale(v3 a, float s) { return v3_make(a.x*s, a.y*s, a.z*s); }
static inline float v3_dot(v3 a, v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline v3 v3_cross(v3 a, v3 b) {
    return v3_make(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
static inline float v3_len(v3 a) { return sqrtf(v3_dot(a,a)); }
static inline v3 v3_norm(v3 a) { float l = v3_len(a); return l > 1e-8f ? v3_scale(a, 1.0f/l) : v3_make(0,0,0); }
static inline float v3_dist(v3 a, v3 b) { return v3_len(v3_sub(a,b)); }

typedef float m4[16];

static inline void m4_identity(m4 m) {
    memset(m, 0, sizeof(m4));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void m4_mul(m4 out, const m4 a, const m4 b) { /* out = a * b */
    m4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            r[c*4+row] = a[0*4+row]*b[c*4+0] + a[1*4+row]*b[c*4+1]
                       + a[2*4+row]*b[c*4+2] + a[3*4+row]*b[c*4+3];
        }
    memcpy(out, r, sizeof(m4));
}

static inline void m4_translate(m4 m, float x, float y, float z) {
    m4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static inline void m4_scale(m4 m, float x, float y, float z) {
    m4_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
}

static inline void m4_rot_x(m4 m, float a) {
    m4_identity(m);
    float c = cosf(a), s = sinf(a);
    m[5] = c; m[9] = -s; m[6] = s; m[10] = c;
}

static inline void m4_rot_y(m4 m, float a) {
    m4_identity(m);
    float c = cosf(a), s = sinf(a);
    m[0] = c; m[8] = s; m[2] = -s; m[10] = c;
}

static inline void m4_rot_z(m4 m, float a) {
    m4_identity(m);
    float c = cosf(a), s = sinf(a);
    m[0] = c; m[4] = -s; m[1] = s; m[5] = c;
}

/* Compose: T * Rz * Ry * Rx * S */
static inline void m4_trs(m4 out, v3 t, v3 s, float rx, float ry, float rz) {
    m4 T, S, X, Y, Z, tmp;
    m4_translate(T, t.x, t.y, t.z);
    m4_scale(S, s.x, s.y, s.z);
    m4_rot_x(X, rx); m4_rot_y(Y, ry); m4_rot_z(Z, rz);
    m4_mul(tmp, Z, Y); m4_mul(out, tmp, X);      /* R = Z*Y*X */
    m4_mul(tmp, out, S); m4_mul(out, T, tmp);    /* T*R*S */
}

static inline void m4_perspective(m4 m, float fov_y, float aspect, float zn, float zf) {
    memset(m, 0, sizeof(m4));
    float f = 1.0f / tanf(fov_y * 0.5f);
    m[0] = f / aspect;
    m[5] = -f;                                 /* flip for Vulkan NDC (y down) */
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = zn * zf / (zn - zf);
}

/* reversed-Z: depth 1 at near plane, 0 at far (use GREATER compare, clear 0) */
static inline void m4_perspective_rz(m4 m, float fov_y, float aspect, float zn, float zf) {
    memset(m, 0, sizeof(m4));
    float f = 1.0f / tanf(fov_y * 0.5f);
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = zn / (zf - zn);
    m[11] = -1.0f;
    m[14] = zn * zf / (zf - zn);
}

static inline void m4_ortho(m4 m, float l, float r, float b, float t, float zn, float zf) {
    memset(m, 0, sizeof(m4));
    m[0] = 2.0f/(r-l); m[5] = 2.0f/(t-b); m[10] = 1.0f/(zn-zf);
    m[12] = -(r+l)/(r-l); m[13] = -(t+b)/(t-b); m[14] = zn/(zn-zf); m[15] = 1.0f;
}

/* reversed-Z ortho (near -> 1, far -> 0) */
static inline void m4_ortho_rz(m4 m, float l, float r, float b, float t, float zn, float zf) {
    memset(m, 0, sizeof(m4));
    m[0] = 2.0f/(r-l); m[5] = 2.0f/(t-b); m[10] = 1.0f/(zf-zn);
    m[12] = -(r+l)/(r-l); m[13] = -(t+b)/(t-b); m[14] = zf/(zf-zn); m[15] = 1.0f;
}

static inline void m4_look_at(m4 m, v3 eye, v3 center, v3 up) {
    v3 f = v3_norm(v3_sub(center, eye));
    v3 s = v3_norm(v3_cross(f, up));
    v3 u = v3_cross(s, f);
    m4_identity(m);
    m[0]=s.x; m[4]=s.y; m[8]=s.z;
    m[1]=u.x; m[5]=u.y; m[9]=u.z;
    m[2]=-f.x; m[6]=-f.y; m[10]=-f.z;
    m[12]=-v3_dot(s,eye); m[13]=-v3_dot(u,eye); m[14]=v3_dot(f,eye);
}

static inline void m4_mul_v3(v3 *out, const m4 m, v3 v) { /* w=1 transform */
    v3 r;
    r.x = m[0]*v.x+m[4]*v.y+m[8]*v.z+m[12];
    r.y = m[1]*v.x+m[5]*v.y+m[9]*v.z+m[13];
    r.z = m[2]*v.x+m[6]*v.y+m[10]*v.z+m[14];
    *out = r;
}

/* full transform; out_w receives clip w */
static inline void m4_mul_v4(v3 *out, float *out_w, const m4 m, v3 v) {
    out->x = m[0]*v.x+m[4]*v.y+m[8]*v.z+m[12];
    out->y = m[1]*v.x+m[5]*v.y+m[9]*v.z+m[13];
    out->z = m[2]*v.x+m[6]*v.y+m[10]*v.z+m[14];
    *out_w = m[3]*v.x+m[7]*v.y+m[11]*v.z+m[15];
}

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float snapf(float x, float step) { return floorf(x/step + 0.5f) * step; }

#endif /* MATH3D_H */
