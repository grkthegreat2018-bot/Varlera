/* export.c - world -> Wavefront OBJ.
 * Built-ins emit their meshgen soup through the instance transform
 * (pos + roty(v*size, rot) — same as world_i.vert). Fused models emit
 * each child's soup in model space then normalize like bake_mesh;
 * sculpted models emit their indexed mv/mf directly. */
#include "export.h"
#include "app.h"
#include "world.h"
#include "model.h"
#include "meshgen.h"
#include "rk.h"
#include <stdio.h>
#include <string.h>

static v3 roty(v3 p, int k) {           /* matches model.c / world_i.vert */
    switch (k & 3) {
    case 1: return v3_make( p.z, p.y, -p.x);
    case 2: return v3_make(-p.x, p.y, -p.z);
    case 3: return v3_make(-p.z, p.y,  p.x);
    }
    return p;
}

typedef struct {
    FILE *f;
    int vbase;                          /* 1-based OBJ index of next vert */
    int nface;
} Obj;

/* one mesh soup through transform xf(v)->world; flat tris, col override=0
 * keeps the vert's baked color (sculpted models) */
static void obj_soup(Obj *o, const Vert *vs, int nv, const float col[3],
                     v3 base, v3 scale, int rot, v3 mdiv) {
    for (int i = 0; i < nv; i++) {
        v3 p = v3_make(vs[i].pos[0], vs[i].pos[1], vs[i].pos[2]);
        v3 n = v3_make(vs[i].nrm[0], vs[i].nrm[1], vs[i].nrm[2]);
        n = roty(n, rot);
        p = v3_make(p.x / mdiv.x, p.y / mdiv.y, p.z / mdiv.z);
        p = v3_make(p.x * scale.x, p.y * scale.y, p.z * scale.z);
        p = v3_add(base, roty(p, rot));
        const float *c = col ? col : vs[i].col;
        fprintf(o->f, "v %.4f %.4f %.4f %.3f %.3f %.3f\n",
                p.x, p.y, p.z, c[0], c[1], c[2]);
        fprintf(o->f, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
    }
    for (int i = 0; i + 2 < nv; i += 3)
        fprintf(o->f, "f %d//%d %d//%d %d//%d\n",
                o->vbase + i,     o->vbase + i,
                o->vbase + i + 1, o->vbase + i + 1,
                o->vbase + i + 2, o->vbase + i + 2);
    o->vbase += nv;
    o->nface += nv / 3;
}

/* built-in shape block: unit mesh -> pos + roty(v*size, rot) */
static void obj_builtin(Obj *o, int shape, const Block *b,
                        v3 base, v3 scale, v3 mdiv) {
    static Vert soup[8192];
    int nv = meshgen_shape(shape, soup, 8192);
    obj_soup(o, soup, nv, b->col, base, scale, b->rot, mdiv);
}

/* block-local vert -> model-normalized unit box -> instance transform */
static void obj_model(Obj *o, const Block *b, const Model *m) {
    if (m->vox) {   /* sculpted: indexed mv/mf, already AABB-centered */
        for (int i = 0; i < m->nvert; i++) {
            v3 u = v3_make(m->mv[i][0] / m->size.x,
                           m->mv[i][1] / m->size.y,
                           m->mv[i][2] / m->size.z);
            v3 p = v3_make(u.x * b->size.x, u.y * b->size.y, u.z * b->size.z);
            p = v3_add(b->pos, roty(p, b->rot));
            fprintf(o->f, "v %.4f %.4f %.4f %.3f %.3f %.3f\n",
                    p.x, p.y, p.z,
                    m->mvcol[i][0] / 255.0f, m->mvcol[i][1] / 255.0f,
                    m->mvcol[i][2] / 255.0f);
        }
        /* flat face normals */
        for (int i = 0; i < m->nface; i++) {
            v3 a = v3_make(m->mv[m->mf[i][0]][0], m->mv[m->mf[i][0]][1],
                           m->mv[m->mf[i][0]][2]);
            v3 c = v3_make(m->mv[m->mf[i][2]][0], m->mv[m->mf[i][2]][1],
                           m->mv[m->mf[i][2]][2]);
            v3 d = v3_make(m->mv[m->mf[i][1]][0], m->mv[m->mf[i][1]][1],
                           m->mv[m->mf[i][1]][2]);
            v3 n = v3_norm(v3_cross(v3_sub(d, a), v3_sub(c, a)));
            n = roty(n, b->rot);
            fprintf(o->f, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
        }
        for (int i = 0; i < m->nface; i++)
            fprintf(o->f, "f %d//%d %d//%d %d//%d\n",
                    o->vbase + m->mf[i][0], o->vbase + i,
                    o->vbase + m->mf[i][1], o->vbase + i,
                    o->vbase + m->mf[i][2], o->vbase + i);
        o->vbase += m->nvert;
        o->nface += m->nface;
        return;
    }
    /* fused: children emit in model space, normalized by m->size */
    for (int i = 0; i < m->nch; i++) {
        const Block *c = &m->ch[i];
        static Vert soup[8192];
        int nv = meshgen_shape(c->shape, soup, 8192);
        for (int v = 0; v < nv; v++) {   /* child local -> model space */
            v3 p = v3_make(soup[v].pos[0] * c->size.x,
                           soup[v].pos[1] * c->size.y,
                           soup[v].pos[2] * c->size.z);
            p = v3_add(c->pos, roty(p, c->rot));
            soup[v].pos[0] = p.x; soup[v].pos[1] = p.y; soup[v].pos[2] = p.z;
            v3 n = roty(v3_make(soup[v].nrm[0], soup[v].nrm[1],
                                soup[v].nrm[2]), c->rot);
            soup[v].nrm[0] = n.x; soup[v].nrm[1] = n.y; soup[v].nrm[2] = n.z;
        }
        obj_soup(o, soup, nv, c->col, b->pos, b->size, b->rot, m->size);
    }
}

int world_export_obj(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    Obj o = { f, 1, 0 };
    fprintf(f, "# varlera world export (%s)\n# verts carry RGB colors\n",
            A.worldName);
    for (int i = 0; i < g_nb; i++) {
        const Block *b = &g_blocks[i];
        if (b->flags & BF_MERGED) continue;     /* drawn via cluster parent */
        fprintf(f, "o blk%u\n", b->id);
        if (b->shape >= RK_MESH_BASE_MODEL) {
            int mid = b->shape - RK_MESH_BASE_MODEL;
            if (mid < g_nmodels) { obj_model(&o, b, &g_models[mid]); continue; }
        }
        obj_builtin(&o, b->shape, b, b->pos, b->size, v3_make(1,1,1));
    }
    fprintf(f, "# %d blocks, %d tris\n", g_nb, o.nface);
    fclose(f);
    return o.nface > 0;
}
