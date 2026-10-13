/* particles.c - fixed-pool particle sim + instanced ghost draw.
 * Presets key off script actions (BOOST->fire+smoke, SPIN->sparks,
 * DEL->debris) and editor feedback (placement dust). */
#include "particles.h"
#include "rk.h"
#include "prof.h"
#include <string.h>

#define PMAX 4096

typedef struct {
    float pos[3], vel[3];
    float life, life0, size, grow;
    uint8_t col[4];             /* rgba; a = peak alpha */
    uint8_t preset, pad[3];
} P;

static P        s_p[PMAX];
static int      s_pn;
static uint32_t s_rng = 0xC0FFEEu;

static float frand(void) {          /* uniform [-1,1] */
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 9) & 0x7FFFFFu) / 4194304.0f - 1.0f;
}
static float frand1(void) {         /* uniform [0,1] */
    return frand() * 0.5f + 0.5f;
}

/* v0 speed bias / spread / gravity(+)buoyancy(-)x22 / size / life s /
   dSize/dt / peak alpha / glow / mesh */
typedef struct {
    float v, sp, g, sz, life, grow;
    uint8_t a, glow, mesh;
} PreDef;
static const PreDef PRE[P_N] = {
    {0.9f, 0.7f,  0.12f, 0.34f, 1.60f,  0.55f, 110, 0, RK_MESH_SPHERE}, /* SMOKE  */
    {1.4f, 0.9f,  0.30f, 0.20f, 0.55f, -0.16f, 255, 1, RK_MESH_CUBE},   /* FIRE   */
    {4.5f, 4.0f, -0.60f, 0.10f, 0.50f, -0.06f, 255, 1, RK_MESH_CUBE},   /* SPARK  */
    {1.8f, 1.6f, -0.35f, 0.16f, 0.90f,  0.05f, 150, 0, RK_MESH_CUBE},   /* DUST   */
    {1.2f, 1.0f,  0.05f, 0.28f, 0.70f,  0.40f, 170, 0, RK_MESH_SPHERE}, /* PUFF   */
    {3.2f, 3.0f, -1.00f, 0.14f, 1.20f,  0.00f, 255, 0, RK_MESH_CUBE},   /* DEBRIS */
};
static const float DEFCOL[P_N][3] = {
    {0.55f, 0.55f, 0.58f},   /* SMOKE  */
    {1.00f, 0.55f, 0.15f},   /* FIRE   */
    {1.00f, 0.90f, 0.40f},   /* SPARK  */
    {0.65f, 0.60f, 0.50f},   /* DUST   */
    {0.90f, 0.90f, 0.92f},   /* PUFF   */
    {0.50f, 0.50f, 0.50f},   /* DEBRIS */
};

void particles_reset(void) { s_pn = 0; }

static void emit1(int preset, v3 at, v3 dir, const float tint[3]) {
    if (s_pn >= PMAX || preset < 0 || preset >= P_N) return;
    const PreDef *r = &PRE[preset];
    P *p = &s_p[s_pn++];
    p->pos[0] = at.x + frand() * 0.15f;
    p->pos[1] = at.y + frand() * 0.10f;
    p->pos[2] = at.z + frand() * 0.15f;
    p->vel[0] = dir.x * r->v + frand() * r->sp;
    p->vel[1] = dir.y * r->v + fabsf(frand()) * r->sp * 0.6f;
    p->vel[2] = dir.z * r->v + frand() * r->sp;
    float lf = r->life * (0.6f + 0.6f * frand1());
    p->life = p->life0 = lf;
    p->size = r->sz * (0.7f + 0.6f * frand1());
    p->grow = r->grow;
    const float *t = tint ? tint : DEFCOL[preset];
    p->col[0] = (uint8_t)(t[0] * 255.0f);
    p->col[1] = (uint8_t)(t[1] * 255.0f);
    p->col[2] = (uint8_t)(t[2] * 255.0f);
    p->col[3] = r->a;
    p->preset = (uint8_t)preset;
}

void particles_burst(int preset, v3 at, const float tint[3], int n) {
    for (int i = 0; i < n; i++) emit1(preset, at, v3_make(0, 0, 0), tint);
}
void particles_burst_dir(int preset, v3 at, v3 dir, const float tint[3], int n) {
    for (int i = 0; i < n; i++) emit1(preset, at, dir, tint);
}

void particles_update(float dt) {
    for (int i = 0; i < s_pn; i++) {
        P *p = &s_p[i];
        p->life -= dt;
        if (p->life <= 0.0f || p->size <= 0.015f) { s_p[i] = s_p[--s_pn]; i--; continue; }
        const PreDef *r = &PRE[p->preset];
        p->vel[1] += r->g * 22.0f * dt;
        p->pos[0] += p->vel[0] * dt;
        p->pos[1] += p->vel[1] * dt;
        p->pos[2] += p->vel[2] * dt;
        p->size += p->grow * dt;
        if (p->pos[1] < p->size * 0.5f) {         /* settle on the grid */
            p->pos[1] = p->size * 0.5f;
            p->vel[0] *= 0.3f; p->vel[1] = 0; p->vel[2] *= 0.3f;
        }
    }
}

void particles_draw(void) {
    /* buckets: glowing cubes (fire/spark), lit cubes (dust/debris),
       lit spheres (smoke/puff) — one ghost call each */
    static RkInst gc[PMAX], lc[PMAX], ls[PMAX];
    int ng = 0, nc = 0, ns = 0;
    for (int i = 0; i < s_pn; i++) {
        const P *p = &s_p[i];
        float fade = (p->life0 - p->life) * 8.0f;
        if (fade > 1.0f) fade = 1.0f;           /* fast ramp-in */
        float out = p->life / (p->life0 * 0.7f);
        if (out > 1.0f) out = 1.0f;
        float a = (float)p->col[3] * fade * out / 255.0f;
        uint32_t fl = PRE[p->preset].glow ? RKIF_NOFOG | RKIF_MAT(TEXM_GLOW) : 0u;
        RkInst in;
        in.pos[0] = p->pos[0]; in.pos[1] = p->pos[1]; in.pos[2] = p->pos[2];
        in.scale[0] = in.scale[1] = in.scale[2] = p->size;
        in.color = rk_rgba((float)p->col[0] / 255.0f, (float)p->col[1] / 255.0f,
                           (float)p->col[2] / 255.0f, a);
        in.flags = fl;
        if (fl) gc[ng++] = in;
        else if (PRE[p->preset].mesh == RK_MESH_SPHERE) ls[ns++] = in;
        else lc[nc++] = in;
    }
    if (ng) rk_ghost(RK_MESH_CUBE,   gc, (uint32_t)ng);
    if (nc) rk_ghost(RK_MESH_CUBE,   lc, (uint32_t)nc);
    if (ns) rk_ghost(RK_MESH_SPHERE, ls, (uint32_t)ns);
    prof_count(PC_PARTICLES, s_pn);
}
