/* model.c - fused-block models: bake children into one mesh (cube faces that are
 * fully covered by a neighbouring cube are skipped), persist as custom assets. */
#include "model.h"
#include "app.h"
#include "edit.h"
#include "sculpt.h"
#include "math3d.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

Model g_models[RK_MAX_MODELS];
int g_nmodels;

/* ---------------- vertex emit ---------------- */

static Vert *pool; static int pn, pmax;

static void ev(v3 p, v3 n, const float c[3], float tex) {
    if (pn >= pmax) return;
    Vert *v = &pool[pn++];
    memcpy(v->pos, p.e, 12); memcpy(v->nrm, n.e, 12); memcpy(v->col, c, 12);
    v->tex = tex;
}
static void emit_quad(v3 a, v3 b, v3 c, v3 d, v3 n, const float col[3], float tex) {
    ev(a,n,col,tex); ev(b,n,col,tex); ev(c,n,col,tex);
    ev(a,n,col,tex); ev(c,n,col,tex); ev(d,n,col,tex);
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
            emit_quad(wc[0], wc[1], wc[2], wc[3], wn, ch->col,
                      ch->tex ? (float)ch->tex : -1.0f);
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
        ev(v3_add(rp, roty(p, rot)), roty(n, rot), ch->col,
           ch->tex ? (float)ch->tex : -1.0f);
    }
}

/* bake rel-positioned children -> mesh id. Output verts are normalized to
 * the [-0.5,0.5] unit box of `size` so instancing by block size restores the
 * true extents (model blocks draw with scale = size). */
static int bake_mesh(const Block *rel, int n, v3 size) {
    static Vert poolbuf[96 * 1024];
    pool = poolbuf; pn = 0; pmax = (int)(sizeof poolbuf / sizeof(Vert));
    for (int i = 0; i < n; i++) {
        if (rel[i].shape == RK_MESH_CUBE) bake_cube(&rel[i], v3_make(0,0,0), rel, n, i);
        else                              bake_shape(&rel[i], v3_make(0,0,0));
    }
    for (int i = 0; i < pn; i++) {
        for (int a = 0; a < 3; a++) {
            float s = size.e[a]; if (s < 0.125f) s = 0.125f;
            poolbuf[i].pos[a] /= s;
        }
    }
    return rk_model_mesh_add(poolbuf, pn);
}

/* topo mesh -> GPU: expand faces to flat-shaded tri soup, normalize to the
 * unit box of `size`, vertex colors preserved */
/* same-material shading: verts welded to one dot share their material id,
   so touching faces box-project continuously and read as one piece */
static int bake_topo(const float (*vx)[3], const uint8_t (*vc)[3],
                     const int8_t *vt,
                     const uint16_t (*fc)[3], int nv, int nf, v3 size) {
    static Vert buf[MODEL_MAX_FACES * 3];
    int n = 0;
    for (int f = 0; f < nf && n + 3 <= MODEL_MAX_FACES * 3; f++) {
        v3 p[3];
        for (int k = 0; k < 3; k++) {
            int vi = fc[f][k];
            p[k] = v3_make(vx[vi][0], vx[vi][1], vx[vi][2]);
        }
        v3 nrm = v3_norm(v3_cross(v3_sub(p[1],p[0]), v3_sub(p[2],p[0])));
        /* a face keeps one material: majority/first non-(-1) wins, so a
           face whose corners carry the same layer shades seamlessly */
        float ft = -1.0f;
        for (int k = 0; k < 3; k++)
            if (vt && vt[fc[f][k]] >= 0) { ft = (float)vt[fc[f][k]]; break; }
        for (int k = 0; k < 3; k++) {
            Vert *v = &buf[n++];
            for (int a = 0; a < 3; a++) {
                float s = size.e[a]; if (s < 0.125f) s = 0.125f;
                v->pos[a] = p[k].e[a] / s;
            }
            memcpy(v->nrm, nrm.e, 12);
            int vi = fc[f][k];
            v->col[0] = vc[vi][0]/255.0f; v->col[1] = vc[vi][1]/255.0f;
            v->col[2] = vc[vi][2]/255.0f;
            v->tex = ft;   /* per-face material keeps flat interp stable */
        }
    }
    return rk_model_mesh_add(buf, n);
}

/* mesh bake for either model kind (children blocks vs sculpted topo) */
static int model_rebake(const Model *m) {
    if (m->vox)
        return bake_topo(m->mv, m->mvcol, m->mvtex, m->mf,
                         m->nvert, m->nface, m->size);
    return bake_mesh(m->ch, m->nch, m->size);
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
    m->mesh = bake_mesh(m->ch, n, m->size);
    if (m->mesh < 0) return -1;
    return g_nmodels++;
}

/* sculpted vertex-edited mesh -> model (verts arrive centered on AABB) */
int model_sculpt_topo(const float (*vx)[3], const uint8_t (*vc)[3],
                      const int8_t *vt, const uint8_t *vl,
                      const uint16_t (*fc)[3], int nv, int nf,
                      const char *name) {
    if (g_nmodels >= RK_MAX_MODELS || nv < 1 || nf < 1 ||
        nv > MODEL_MAX_VERTS || nf > MODEL_MAX_FACES) return -1;
    /* bounds from the (centered) verts */
    v3 hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < nv; i++)
        for (int a = 0; a < 3; a++)
            if (vx[i][a] > hi.e[a]) hi.e[a] = vx[i][a];
    Model *m = &g_models[g_nmodels];
    memset(m, 0, sizeof *m);
    if (name && name[0])
        snprintf(m->name, sizeof m->name, "%.11s", name);
    else snprintf(m->name, sizeof m->name, "SCULPT %d", g_nmodels + 1);
    m->nch = 0; m->vox = 1;
    m->size = v3_make(fmaxf(hi.x*2,0.125f), fmaxf(hi.y*2,0.125f),
                      fmaxf(hi.z*2,0.125f));
    m->nvert = nv; m->nface = nf;
    memcpy(m->mv, vx, (size_t)nv * sizeof m->mv[0]);
    memcpy(m->mvcol, vc, (size_t)nv * sizeof m->mvcol[0]);
    if (vt) memcpy(m->mvtex, vt, (size_t)nv);
    else    memset(m->mvtex, -1, (size_t)nv);
    if (vl) memcpy(m->mvlock, vl, (size_t)nv);
    memcpy(m->mf, fc, (size_t)nf * sizeof m->mf[0]);
    m->mesh = bake_topo(m->mv, m->mvcol, m->mvtex, m->mf, nv, nf, m->size);
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
        g_models[i].mesh = model_rebake(&g_models[i]);
    rk_models_commit();
    models_save(NULL);
}

/* rebuild every model mesh in-place (mesh ids stay dense: 16+i) */
void models_rebake_all(void) {
    rk_models_reset();
    for (int i = 0; i < g_nmodels; i++)
        g_models[i].mesh = model_rebake(&g_models[i]);
    rk_models_commit();
}

static const char *def_path(void) { return "models.bw"; }
static int s_lastver = 0;               /* file format of the loaded save */

int models_save(const char *path) {
    FILE *f = fopen(path ? path : def_path(), "wb");
    if (!f) return 0;
    uint32_t hdr[3] = {0x44574D42u /* 'BWMD' */, 7, (uint32_t)g_nmodels};
    fwrite(hdr, 4, 3, f);
    for (int i = 0; i < g_nmodels; i++) {
        Model *m = &g_models[i];
        fwrite(m->name, 1, 16, f);
        int nrec = m->vox ? -2 : m->nch;      /* -2 = sculpted topo mesh */
        fwrite(&nrec, 4, 1, f);
        fwrite(m->size.e, 4, 3, f);
        int32_t flags = m->adv ? 1 : 0;        /* bit0 = premade asset */
        fwrite(&flags, 4, 1, f);
        if (m->vox) {
            fwrite(&m->nvert, 4, 1, f);
            fwrite(&m->nface, 4, 1, f);
            fwrite(m->mv, 12, m->nvert, f);
            fwrite(m->mvcol, 3, m->nvert, f);
            fwrite(m->mf, 6, m->nface, f);
            fwrite(m->mvtex, 1, m->nvert, f);
            fwrite(m->mvlock, 1, m->nvert, f);
        } else fwrite(m->ch, sizeof(Block), m->nch, f);
    }
    fclose(f);
    return 1;
}

#define MODELS_V1_BLKSZ 40      /* v1: no trig/act */
#define MODELS_V2_BLKSZ 44      /* v2: +trig/act; no texm/alpha/id/name */

#define WH_BITS 17
#define WH_SIZE (1 << WH_BITS)

/* welded tri-soup (world units) -> topo arrays centered on the AABB center.
   returns 0 ok / -1 nothing welded */
static int weld_topo(const Vert *soup, int ns, Model *m) {
    static int htab[WH_SIZE];            /* vert idx + 1, 0 = empty */
    memset(htab, 0, sizeof htab);
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < ns; i++)
        for (int a = 0; a < 3; a++) {
            if (soup[i].pos[a] < lo.e[a]) lo.e[a] = soup[i].pos[a];
            if (soup[i].pos[a] > hi.e[a]) hi.e[a] = soup[i].pos[a];
        }
    v3 ctr = v3_scale(v3_add(lo, hi), 0.5f);
    int nv = 0, nf = 0;
    for (int i = 0; i + 2 < ns + 1 && i + 3 <= ns; i += 3) {
        uint16_t ix[3]; int ok = 1;
        for (int k = 0; k < 3; k++) {
            const Vert *s = &soup[i + k];
            v3 p = v3_make(s->pos[0]-ctr.x, s->pos[1]-ctr.y, s->pos[2]-ctr.z);
            int qx = (int)lrintf(p.x * 4096.0f);
            int qy = (int)lrintf(p.y * 4096.0f);
            int qz = (int)lrintf(p.z * 4096.0f);
            uint32_t h = (uint32_t)(qx*73856093 ^ qy*19349663 ^ qz*83492791)
                         & (WH_SIZE - 1);
            int f = -1;
            for (;;) {
                int e = htab[h];
                if (!e) break;
                int j = e - 1;
                if ((int)lrintf(m->mv[j][0]*4096.0f) == qx &&
                    (int)lrintf(m->mv[j][1]*4096.0f) == qy &&
                    (int)lrintf(m->mv[j][2]*4096.0f) == qz) { f = j; break; }
                h = (h + 1) & (WH_SIZE - 1);
            }
            if (f < 0) {
                if (nv >= MODEL_MAX_VERTS) { ok = 0; break; }
                f = nv++;
                m->mv[f][0] = p.x; m->mv[f][1] = p.y; m->mv[f][2] = p.z;
                for (int c = 0; c < 3; c++) {
                    float cl = s->col[c] * 255.0f;
                    m->mvcol[f][c] = (uint8_t)(cl < 0 ? 0 : cl > 255 ? 255 : cl);
                }
                htab[h] = f + 1;
            }
            ix[k] = (uint16_t)f;
        }
        if (!ok) break;
        if (ix[0] == ix[1] || ix[1] == ix[2] || ix[0] == ix[2]) continue;
        if (nf >= MODEL_MAX_FACES) break;
        memcpy(m->mf[nf], ix, 6); nf++;
    }
    if (!nv || !nf) return -1;
    m->nvert = nv; m->nface = nf;
    memset(m->mvtex, -1, (size_t)nv);
    memset(m->mvlock, 0, (size_t)nv);
    m->size = v3_make(fmaxf(hi.x-lo.x,0.125f), fmaxf(hi.y-lo.y,0.125f),
                      fmaxf(hi.z-lo.z,0.125f));
    return 0;
}

int models_load(const char *path) {
    FILE *f = fopen(path ? path : def_path(), "rb");
    if (!f) return 0;
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != 0x44574D42u ||
        (hdr[1] < 1 || hdr[1] > 7)) { fclose(f); return 0; }
    int ver = (int)hdr[1];
    s_lastver = ver;
    int n = (int)hdr[2];
    if (n > RK_MAX_MODELS) n = RK_MAX_MODELS;
    for (int i = 0; i < n; i++) {
        Model *m = &g_models[g_nmodels];
        memset(m, 0, sizeof *m);
        if (fread(m->name, 1, 16, f) != 16) break;
        if (fread(&m->nch, 4, 1, f) != 1) break;
        if (fread(m->size.e, 4, 3, f) != 3) break;
        if (ver >= 7) {                          /* per-model flags */
            int32_t fl = 0;
            if (fread(&fl, 4, 1, f) != 1) break;
            m->adv = (fl & 1) != 0;
        }
        if (m->nch == -2 && ver >= 6) {          /* sculpted topo mesh */
            m->vox = 1; m->nch = 0;
            if (fread(&m->nvert, 4, 1, f) != 1) break;
            if (fread(&m->nface, 4, 1, f) != 1) break;
            if (m->nvert < 1 || m->nvert > MODEL_MAX_VERTS ||
                m->nface < 1 || m->nface > MODEL_MAX_FACES) break;
            if (fread(m->mv, 12, m->nvert, f) != (size_t)m->nvert) break;
            if (fread(m->mvcol, 3, m->nvert, f) != (size_t)m->nvert) break;
            if (fread(m->mf, 6, m->nface, f) != (size_t)m->nface) break;
            memset(m->mvtex, -1, (size_t)m->nvert);
            memset(m->mvlock, 0, (size_t)m->nvert);
            if (ver >= 7) {
                if (fread(m->mvtex, 1, m->nvert, f) != (size_t)m->nvert) break;
                if (fread(m->mvlock, 1, m->nvert, f) != (size_t)m->nvert) break;
            }
        } else if (m->nch == -1 && (ver == 4 || ver == 5)) {
            /* legacy voxel/density sculpts -> bake + weld into topo */
            m->vox = 1; m->nch = 0;
            static float den[SC_VOX_N];
            static uint8_t col[SC_VOX_N][3];
            if (ver == 5) {
                static uint8_t v5[SC_VOX_N][4];
                if (fread(v5, 4, SC_VOX_N, f) != SC_VOX_N) break;
                sculpt_vox_unpack(v5, den, col);
            } else {
                static uint16_t v4[4096];
                if (fread(v4, 2, 4096, f) != 4096) break;
                sculpt_vox_unpack16(v4, den, col);
            }
            static Vert soup[SC_MAX_VERTS];
            int ns = sculpt_bake_verts(den, col,
                                       (int)fmaxf(m->size.x, 1.0f),
                                       soup, SC_MAX_VERTS);
            if (ns <= 0 || weld_topo(soup, ns, m) < 0) break;
        } else {
            if (m->nch < 1 || m->nch > MODEL_MAX_CHILDREN) break;
            if (ver < 3) {
                int rec = ver == 1 ? MODELS_V1_BLKSZ : MODELS_V2_BLKSZ;
                uint8_t buf[MODELS_V2_BLKSZ]; int ok = 1;
                for (int c = 0; c < m->nch; c++) {
                    if (fread(buf, rec, 1, f) != 1) { ok = 0; break; }
                    world_block_migrate(&m->ch[c], buf, ver);
                    m->ch[c].id = 0;            /* children don't need ids */
                }
                if (!ok) break;
            } else if (fread(m->ch, sizeof(Block), m->nch, f) != (size_t)m->nch) break;
        }
        m->mesh = model_rebake(m);
        if (m->mesh < 0) break;
        g_nmodels++;
    }
    fclose(f);
    rk_models_commit();
    return 1;
}

/* ---------------- premade assets (PROPS tab) ---------------- */

static void mkchild(Block *b, float x, float y, float z,
                    float sx, float sy, float sz, int shape,
                    float r, float g, float bl) {
    memset(b, 0, sizeof *b);
    b->pos = v3_make(x, y, z);
    b->size = v3_make(sx, sy, sz);
    b->shape = (uint8_t)shape;
    b->col[0] = r; b->col[1] = g; b->col[2] = bl;
    b->alpha = 1.0f;
    b->texm = TEXM_LOOP;
}

/* official-looking multi-part assets; baked once on first run (adv=1) */
void models_bake_builtins(void) {
    static const float WOOD[3]  = {0.55f, 0.35f, 0.20f};
    static const float DARK[3]  = {0.24f, 0.26f, 0.29f};
    static const float LEAF[3]  = {0.25f, 0.65f, 0.30f};
    static const float SKIN[3]  = {0.95f, 0.65f, 0.45f};
    static const float SHIRT[3] = {0.25f, 0.55f, 0.95f};
    static const float PANTS[3] = {0.35f, 0.30f, 0.50f};
    static const float STONE[3] = {0.55f, 0.58f, 0.62f};
    static const float GOLD[3]  = {0.95f, 0.80f, 0.25f};
    static const float PINK[3]  = {0.95f, 0.55f, 0.70f};
    static const float CREAM[3] = {0.90f, 0.92f, 0.95f};
    Block ch[16];
#define NCH(a) (Block*)ch, (a)
    int i;

    /* CRATE: wooden body + corner posts + lid band */
    i = 0;
    mkchild(&ch[i++], 0, 0, 0,        0.9f, 0.9f, 0.9f,  RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++],-0.40f,0,-0.40f, 0.18f,0.9f, 0.18f, RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.40f,0,-0.40f, 0.18f,0.9f, 0.18f, RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++],-0.40f,0, 0.40f, 0.18f,0.9f, 0.18f, RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.40f,0, 0.40f, 0.18f,0.9f, 0.18f, RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0, 0.44f, 0,    1.0f, 0.10f,1.0f,  RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"CRATE"); }

    /* TABLE: top + 4 legs */
    i = 0;
    mkchild(&ch[i++], 0, 0.45f, 0,    1.6f, 0.12f,1.0f,  RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++],-0.68f,-0.05f,-0.40f,0.14f,0.9f,0.14f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.68f,-0.05f,-0.40f,0.14f,0.9f,0.14f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++],-0.68f,-0.05f, 0.40f,0.14f,0.9f,0.14f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.68f,-0.05f, 0.40f,0.14f,0.9f,0.14f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"TABLE"); }

    /* CHAIR: seat + back + 4 legs */
    i = 0;
    mkchild(&ch[i++], 0, 0.05f, 0,    0.55f,0.10f,0.55f, RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++], 0, 0.45f,-0.23f,0.55f,0.75f,0.09f, RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++],-0.20f,-0.30f,-0.20f,0.09f,0.7f,0.09f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.20f,-0.30f,-0.20f,0.09f,0.7f,0.09f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++],-0.20f,-0.30f, 0.20f,0.09f,0.7f,0.09f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.20f,-0.30f, 0.20f,0.09f,0.7f,0.09f,RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"CHAIR"); }

    /* PERSON: head + torso + arms + legs (Blocksworld-style humanoid) */
    i = 0;
    mkchild(&ch[i++], 0, 0.62f, 0,    0.30f,0.30f,0.30f, RK_MESH_SPHERE, SKIN[0],SKIN[1],SKIN[2]);
    mkchild(&ch[i++], 0, 0.18f, 0,    0.36f,0.55f,0.20f, RK_MESH_CUBE, SHIRT[0],SHIRT[1],SHIRT[2]);
    mkchild(&ch[i++],-0.26f,0.18f,0,  0.10f,0.45f,0.10f, RK_MESH_CUBE, SKIN[0],SKIN[1],SKIN[2]);
    mkchild(&ch[i++], 0.26f,0.18f,0,  0.10f,0.45f,0.10f, RK_MESH_CUBE, SKIN[0],SKIN[1],SKIN[2]);
    mkchild(&ch[i++],-0.10f,-0.35f,0, 0.14f,0.55f,0.14f, RK_MESH_CUBE, PANTS[0],PANTS[1],PANTS[2]);
    mkchild(&ch[i++], 0.10f,-0.35f,0, 0.14f,0.55f,0.14f, RK_MESH_CUBE, PANTS[0],PANTS[1],PANTS[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"PERSON"); }

    /* TREE: trunk + leaf cluster */
    i = 0;
    mkchild(&ch[i++], 0,-0.15f,0,     0.18f,0.75f,0.18f, RK_MESH_CYLINDER, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++], 0, 0.45f, 0,    0.85f,0.65f,0.85f, RK_MESH_SPHERE, LEAF[0],LEAF[1],LEAF[2]);
    mkchild(&ch[i++], 0.32f,0.28f,0.18f,0.55f,0.45f,0.55f,RK_MESH_SPHERE, LEAF[0]*0.9f,LEAF[1]*0.9f,LEAF[2]*0.9f);
    mkchild(&ch[i++],-0.30f,0.32f,-0.15f,0.6f,0.5f,0.6f, RK_MESH_SPHERE, LEAF[0]*1.1f,LEAF[1],LEAF[2]*1.05f);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"TREE"); }

    /* FLOWER: stem + center + petals */
    i = 0;
    mkchild(&ch[i++], 0,-0.20f,0,     0.07f,0.65f,0.07f, RK_MESH_CYLINDER, LEAF[0],LEAF[1],LEAF[2]);
    mkchild(&ch[i++], 0, 0.18f, 0,    0.16f,0.16f,0.16f, RK_MESH_SPHERE, GOLD[0],GOLD[1],GOLD[2]);
    mkchild(&ch[i++], 0.15f,0.18f,0,  0.20f,0.08f,0.10f, RK_MESH_SPHERE, PINK[0],PINK[1],PINK[2]);
    mkchild(&ch[i++],-0.15f,0.18f,0,  0.20f,0.08f,0.10f, RK_MESH_SPHERE, PINK[0],PINK[1],PINK[2]);
    mkchild(&ch[i++], 0, 0.18f, 0.15f,0.10f,0.08f,0.20f, RK_MESH_SPHERE, PINK[0],PINK[1],PINK[2]);
    mkchild(&ch[i++], 0, 0.18f,-0.15f,0.10f,0.08f,0.20f, RK_MESH_SPHERE, PINK[0],PINK[1],PINK[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"FLOWER"); }

    /* ARCH: two posts + top beam */
    i = 0;
    mkchild(&ch[i++],-0.55f,-0.10f,0, 0.22f,1.0f,0.22f,  RK_MESH_CUBE, STONE[0],STONE[1],STONE[2]);
    mkchild(&ch[i++], 0.55f,-0.10f,0, 0.22f,1.0f,0.22f,  RK_MESH_CUBE, STONE[0],STONE[1],STONE[2]);
    mkchild(&ch[i++], 0, 0.50f, 0,    1.35f,0.22f,0.26f, RK_MESH_CUBE, STONE[0]*0.9f,STONE[1]*0.9f,STONE[2]*0.9f);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"ARCH"); }

    /* LAMP: post + glowing head */
    i = 0;
    mkchild(&ch[i++], 0,-0.20f,0,     0.10f,0.85f,0.10f, RK_MESH_CYLINDER, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0, 0.32f, 0,    0.30f,0.30f,0.30f, RK_MESH_SPHERE, GOLD[0],GOLD[1],GOLD[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"LAMP"); }

    /* FENCE: posts + rails */
    i = 0;
    mkchild(&ch[i++],-0.45f,-0.15f,0, 0.09f,0.7f,0.09f,  RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0.45f,-0.15f,0, 0.09f,0.7f,0.09f,  RK_MESH_CUBE, DARK[0],DARK[1],DARK[2]);
    mkchild(&ch[i++], 0, 0.05f, 0,    0.95f,0.09f,0.06f, RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    mkchild(&ch[i++], 0,-0.25f, 0,    0.95f,0.09f,0.06f, RK_MESH_CUBE, WOOD[0],WOOD[1],WOOD[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"FENCE"); }

    /* PILLAR: base + shaft + capital */
    i = 0;
    mkchild(&ch[i++], 0,-0.42f,0,     0.4f, 0.14f,0.4f,  RK_MESH_CUBE, STONE[0],STONE[1],STONE[2]);
    mkchild(&ch[i++], 0,-0.02f,0,     0.26f,0.7f, 0.26f, RK_MESH_CYLINDER, CREAM[0],CREAM[1],CREAM[2]);
    mkchild(&ch[i++], 0, 0.40f, 0,    0.4f, 0.14f,0.4f,  RK_MESH_CUBE, STONE[0],STONE[1],STONE[2]);
    i = model_bake(NCH(i)); if (i >= 0) { g_models[i].adv = 1; snprintf(g_models[i].name,16,"PILLAR"); }
#undef NCH
    rk_models_commit();
}

void models_init(void) {
    models_load(NULL);
    /* stock the PROPS tab: fresh install, or a pre-v7 save upgrading once
       (v7 saves with deleted premades stay deleted) */
    if (s_lastver < 7) {
        models_bake_builtins();
        models_save(NULL);
    }
}
