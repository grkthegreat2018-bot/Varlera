/* model.c - fused-block models: bake children into one mesh (cube faces that are
 * fully covered by a neighbouring cube are skipped), persist as custom assets. */
#include "model.h"
#include "app.h"
#include "edit.h"
#include "math3d.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

Model g_models[RK_MAX_MODELS];
int g_nmodels;

/* ---------------- vertex emit ---------------- */

static Vert *pool; static int pn, pmax;

static void ev(v3 p, v3 n, const float c[3]) {
    if (pn >= pmax) return;
    Vert *v = &pool[pn++];
    memcpy(v->pos, p.e, 12); memcpy(v->nrm, n.e, 12); memcpy(v->col, c, 12);
}
static void emit_quad(v3 a, v3 b, v3 c, v3 d, v3 n, const float col[3]) {
    ev(a,n,col); ev(b,n,col); ev(c,n,col);
    ev(a,n,col); ev(c,n,col); ev(d,n,col);
}

/* rotate about Y in 90-degree steps — matches world_i.vert rotY() */
static v3 roty(v3 p, int k) {
    switch (k & 3) {
    case 1: return v3_make( p.z, p.y, -p.x);
    case 2: return v3_make(-p.x, p.y, -p.z);
    case 3: return v3_make(-p.z, p.y,  p.x);
    }
    return p;
}

/* ---------------- cube with covered-face culling ---------------- */

static const int CUBE_F[6][4] = {   /* unit-cube vert orders from gen_cube */
    {4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4}
};
static const v3 CUBE_N[6] = {
    {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}
};
static v3 cube_vert(int i) {  /* matches gen_cube's v[] order in rk.c */
    static const v3 c[8] = {
        {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
        {-0.5f,-0.5f, 0.5f},{0.5f,-0.5f, 0.5f},{0.5f,0.5f, 0.5f},{-0.5f,0.5f, 0.5f}
    };
    return c[i & 7];
}

/* is this face fully covered by another cube child? */
static int face_covered(v3 wc[4], v3 wn, const Block *rel, int n, int self) {
    for (int j = 0; j < n; j++) {
        if (j == self || rel[j].shape != RK_MESH_CUBE) continue;
        v3 je = block_eff_size(&rel[j]);
        int all = 1;
        for (int k = 0; k < 4 && all; k++) {
            v3 p = v3_add(wc[k], v3_scale(wn, 0.02f));
            for (int a = 0; a < 3; a++)
                if (fabsf(p.e[a] - rel[j].pos.e[a]) >= je.e[a]*0.5f + 0.01f) { all = 0; break; }
        }
        if (all) return 1;
    }
    return 0;
}



static void bake_cube(const Block *ch, v3 pivot, const Block *rel, int n, int self) {
    v3 rp = v3_sub(ch->pos, pivot);
    int rot = ch->rot & 3;
    v3 wc[4], wn;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            v3 u = cube_vert(CUBE_F[f][k]);
            v3 p = v3_make(u.x*ch->size.x, u.y*ch->size.y, u.z*ch->size.z);
            wc[k] = v3_add(rp, roty(p, rot));
        }
        wn = roty(CUBE_N[f], rot);
        if (!face_covered(wc, wn, rel, n, self))
            emit_quad(wc[0], wc[1], wc[2], wc[3], wn, ch->col);
    }
}

static void bake_shape(const Block *ch, v3 pivot) {
    static Vert tmp[8192];
    int tn = rk_gen_shape(ch->shape, tmp, 8192);
    v3 rp = v3_sub(ch->pos, pivot);
    int rot = ch->rot & 3;
    for (int i = 0; i < tn; i++) {
        v3 p = v3_make(tmp[i].pos[0]*ch->size.x, tmp[i].pos[1]*ch->size.y,
                       tmp[i].pos[2]*ch->size.z);
        v3 n = v3_make(tmp[i].nrm[0], tmp[i].nrm[1], tmp[i].nrm[2]);
        ev(v3_add(rp, roty(p, rot)), roty(n, rot), ch->col);
    }
}

/* bake rel-positioned children -> mesh id */
static int bake_mesh(const Block *rel, int n) {
    static Vert poolbuf[96 * 1024];
    pool = poolbuf; pn = 0; pmax = (int)(sizeof poolbuf / sizeof(Vert));
    for (int i = 0; i < n; i++) {
        if (rel[i].shape == RK_MESH_CUBE) bake_cube(&rel[i], v3_make(0,0,0), rel, n, i);
        else                              bake_shape(&rel[i], v3_make(0,0,0));
    }
    return rk_model_mesh_add(poolbuf, pn);
}

/* ---------------- public api ---------------- */

int model_bake(const Block *children, int n) {
    if (g_nmodels >= RK_MAX_MODELS || n < 1 || n > MODEL_MAX_CHILDREN) return -1;
    /* pivot + bounds from effective AABBs */
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < n; i++) {
        v3 es = block_eff_size(&children[i]);
        for (int a = 0; a < 3; a++) {
            float l = children[i].pos.e[a] - es.e[a]*0.5f;
            float h = children[i].pos.e[a] + es.e[a]*0.5f;
            if (l < lo.e[a]) lo.e[a] = l;
            if (h > hi.e[a]) hi.e[a] = h;
        }
    }
    v3 pivot = v3_scale(v3_add(lo, hi), 0.5f);

    Model *m = &g_models[g_nmodels];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "MODEL %d", g_nmodels + 1);
    m->nch = n;
    m->size = v3_sub(hi, lo);
    for (int i = 0; i < n; i++) {
        m->ch[i] = children[i];
        m->ch[i].pos = v3_sub(children[i].pos, pivot);
    }
    m->mesh = bake_mesh(m->ch, n);
    if (m->mesh < 0) return -1;
    return g_nmodels++;
}

/* unfuse every placed instance of model `mid`, then drop the record and
   re-bake all meshes so mesh ids stay dense (16+i matches g_models[i]) */
void model_delete(int mid) {
    if (mid < 0 || mid >= g_nmodels) return;
    world_push_undo();
    int mesh = g_models[mid].mesh;
    for (int i = 0; i < g_nb; i++)
        while (i < g_nb && g_blocks[i].shape == mesh) unfuse_block(i);
    memmove(&g_models[mid], &g_models[mid+1],
            sizeof(Model) * (g_nmodels - mid - 1));
    g_nmodels--;
    for (int i = 0; i < g_nb; i++)
        if (g_blocks[i].shape > mesh &&
            g_blocks[i].shape < RK_MESH_BASE_MODEL + RK_MAX_MODELS)
            g_blocks[i].shape--;
    for (int i = 0; i < A.nClip; i++)
        if (A.clip[i].shape > mesh &&
            A.clip[i].shape < RK_MESH_BASE_MODEL + RK_MAX_MODELS)
            A.clip[i].shape--;
    rk_models_reset();
    for (int i = 0; i < g_nmodels; i++)
        g_models[i].mesh = bake_mesh(g_models[i].ch, g_models[i].nch);
    rk_models_commit();
    models_save(NULL);
}

static const char *def_path(void) { return "models.bw"; }

int models_save(const char *path) {
    FILE *f = fopen(path ? path : def_path(), "wb");
    if (!f) return 0;
    uint32_t hdr[3] = {0x44574D42u /* 'BWMD' */, 2, (uint32_t)g_nmodels};
    fwrite(hdr, 4, 3, f);
    for (int i = 0; i < g_nmodels; i++) {
        Model *m = &g_models[i];
        fwrite(m->name, 1, 16, f);
        fwrite(&m->nch, 4, 1, f);
        fwrite(m->size.e, 4, 3, f);
        fwrite(m->ch, sizeof(Block), m->nch, f);
    }
    fclose(f);
    return 1;
}

#define MODELS_V1_BLKSZ 40      /* Block v1 had no trig/act */

int models_load(const char *path) {
    FILE *f = fopen(path ? path : def_path(), "rb");
    if (!f) return 0;
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != 0x44574D42u ||
        (hdr[1] != 1 && hdr[1] != 2)) { fclose(f); return 0; }
    int v1 = hdr[1] == 1;
    int n = (int)hdr[2];
    if (n > RK_MAX_MODELS) n = RK_MAX_MODELS;
    for (int i = 0; i < n; i++) {
        Model *m = &g_models[g_nmodels];
        memset(m, 0, sizeof *m);
        if (fread(m->name, 1, 16, f) != 16) break;
        if (fread(&m->nch, 4, 1, f) != 1) break;
        if (fread(m->size.e, 4, 3, f) != 3) break;
        if (m->nch < 1 || m->nch > MODEL_MAX_CHILDREN) break;
        if (v1) {
            uint8_t rec[MODELS_V1_BLKSZ]; int ok = 1;
            for (int c = 0; c < m->nch; c++) {
                if (fread(rec, MODELS_V1_BLKSZ, 1, f) != 1) { ok = 0; break; }
                memcpy(&m->ch[c], rec, MODELS_V1_BLKSZ);
                m->ch[c].trig = 0; m->ch[c].act = 0;
            }
            if (!ok) break;
        } else if (fread(m->ch, sizeof(Block), m->nch, f) != (size_t)m->nch) break;
        m->mesh = bake_mesh(m->ch, m->nch);
        if (m->mesh < 0) break;
        g_nmodels++;
    }
    fclose(f);
    rk_models_commit();
    return 1;
}

void models_init(void) { models_load(NULL); }
