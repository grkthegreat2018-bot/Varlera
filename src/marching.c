/* marching.c - legacy density-field bake (models.bw <= v5 migration).
 * Marching-tetrahedra surface extraction over the SC_N^3 voxel field,
 * plus the packed voxel payload unpackers. Public entry points are
 * declared in sculpt.h (sculpt_bake_verts / sculpt_vox_unpack*). */
#include "sculpt.h"
#include <string.h>

#define SN   SC_N
#define SNV  SC_VOX_N
#define ISO  0.5f

static int vidx(int x, int y, int z) { return (z*SN + y)*SN + x; }
static int inb(int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 && x < SN && y < SN && z < SN;
}
static const int TETS[6][4] = {
    {0,1,3,7},{0,1,5,7},{0,2,3,7},{0,2,6,7},{0,4,5,7},{0,4,6,7}
};
static v3 ngrad(const float *den, int x, int y, int z) {
    #define DN(a,b,c) (inb(a,b,c) ? den[vidx(a,b,c)] : 0.0f)
    return v3_make(DN(x+1,y,z) - DN(x-1,y,z),
                   DN(x,y+1,z) - DN(x,y-1,z),
                   DN(x,y,z+1) - DN(x,y,z-1));
    #undef DN
}
typedef struct { v3 p; float c[3]; v3 n; } MV;
static MV ex(const float *den, const uint8_t (*col)[3],
             int ax, int ay, int az, int bx, int by, int bz,
             v3 pa, v3 pb, v3 ga, v3 gb, v3 faceN) {
    float da = den[vidx(ax,ay,az)], db = den[vidx(bx,by,bz)];
    float t = (ISO - da) / (db - da);
    if (t < 0) t = 0; if (t > 1) t = 1;
    MV r;
    r.p = v3_add(pa, v3_scale(v3_sub(pb, pa), t));
    for (int i = 0; i < 3; i++)
        r.c[i] = (col[vidx(ax,ay,az)][i] +
                  (col[vidx(bx,by,bz)][i] - col[vidx(ax,ay,az)][i]) * t) / 255.0f;
    v3 g = v3_scale(v3_add(ga, v3_scale(v3_sub(gb, ga), t)), -1.0f);
    r.n = v3_dot(g, g) > 1e-8f ? v3_norm(g) : faceN;
    return r;
}
static void etri(Vert *o, int *n, int max, MV a, MV b, MV c, v3 emptySide) {
    if (*n + 3 > max) return;
    v3 fn = v3_cross(v3_sub(b.p, a.p), v3_sub(c.p, a.p));
    v3 cc = v3_scale(v3_add(v3_add(a.p, b.p), c.p), 1.0f/3.0f);
    if (v3_dot(fn, v3_sub(emptySide, cc)) < 0) { MV t = b; b = c; c = t; }
    MV vs[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        Vert *v = &o[(*n)++];
        memcpy(v->pos, vs[i].p.e, 12);
        memcpy(v->nrm, vs[i].n.e, 12);
        memcpy(v->col, vs[i].c, 12);
    }
}
static void mtet(Vert *o, int *n, int max,
                 const int nd[4][3], v3 cp[4], v3 cg[4],
                 const float *den, const uint8_t (*col)[3]) {
    int m = 0;
    for (int i = 0; i < 4; i++)
        if (den[vidx(nd[i][0], nd[i][1], nd[i][2])] >= ISO) m |= 1 << i;
    if (!m || m == 15) return;
    int sol[4], emp[4], ns = 0, ne = 0;
    for (int i = 0; i < 4; i++)
        (m & (1<<i)) ? (void)(sol[ns++]=i) : (void)(emp[ne++]=i);
    v3 eside = v3_make(0,0,0);
    for (int i = 0; i < ne; i++) eside = v3_add(eside, cp[emp[i]]);
    eside = v3_scale(eside, 1.0f/ne);
    v3 fn = v3_norm(v3_cross(v3_sub(cp[1],cp[0]), v3_sub(cp[2],cp[0])));
    if (ns == 1 || ns == 3) {
        int x  = ns == 1 ? sol[0] : emp[0];
        int o1 = ns == 1 ? emp[0] : sol[0];
        int o2 = ns == 1 ? emp[1] : sol[1];
        int o3 = ns == 1 ? emp[2] : sol[2];
        MV a = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o1][0],nd[o1][1],nd[o1][2], cp[x], cp[o1], cg[x], cg[o1], fn);
        MV b = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o2][0],nd[o2][1],nd[o2][2], cp[x], cp[o2], cg[x], cg[o2], fn);
        MV c = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o3][0],nd[o3][1],nd[o3][2], cp[x], cp[o3], cg[x], cg[o3], fn);
        etri(o, n, max, a, b, c, eside);
    } else {
        int a = sol[0], b = sol[1], c = emp[0], d = emp[1];
        MV vac = ex(den, col, nd[a][0],nd[a][1],nd[a][2],
                    nd[c][0],nd[c][1],nd[c][2], cp[a], cp[c], cg[a], cg[c], fn);
        MV vad = ex(den, col, nd[a][0],nd[a][1],nd[a][2],
                    nd[d][0],nd[d][1],nd[d][2], cp[a], cp[d], cg[a], cg[d], fn);
        MV vbc = ex(den, col, nd[b][0],nd[b][1],nd[b][2],
                    nd[c][0],nd[c][1],nd[c][2], cp[b], cp[c], cg[b], cg[c], fn);
        MV vbd = ex(den, col, nd[b][0],nd[b][1],nd[b][2],
                    nd[d][0],nd[d][1],nd[d][2], cp[b], cp[d], cg[b], cg[d], fn);
        etri(o, n, max, vac, vad, vbd, eside);
        etri(o, n, max, vac, vbd, vbc, eside);
    }
}

int sculpt_bake_verts(const float *den, const uint8_t (*col)[3], int size,
                      Vert *out, int max) {
    int nv = 0;
    float p = (float)size / (SN - 1), h = size * 0.5f;
    for (int z = 0; z < SN-1; z++)
    for (int y = 0; y < SN-1; y++)
    for (int x = 0; x < SN-1; x++) {
        int inside = 0, allin = 1;
        for (int c = 0; c < 8; c++)
            if (den[vidx(x+(c&1), y+((c>>1)&1), z+((c>>2)&1))] >= ISO)
                inside = 1;
            else allin = 0;
        if (!inside || allin) continue;
        for (int t = 0; t < 6; t++) {
            int nd[4][3]; v3 tp[4], tg[4];
            for (int i = 0; i < 4; i++) {
                int c = TETS[t][i];
                nd[i][0] = x + (c&1);
                nd[i][1] = y + ((c>>1)&1);
                nd[i][2] = z + ((c>>2)&1);
                tp[i] = v3_make(nd[i][0]*p - h, nd[i][1]*p - h,
                                nd[i][2]*p - h);
                tg[i] = ngrad(den, nd[i][0], nd[i][1], nd[i][2]);
            }
            mtet(out, &nv, max, nd, tp, tg, den, col);
        }
    }
    return nv;
}

void sculpt_vox_unpack(const uint8_t (*vox4)[4], float *den, uint8_t (*col)[3]) {
    for (int i = 0; i < SNV; i++) {
        den[i] = vox4[i][3] / 255.0f;
        col[i][0] = vox4[i][0]; col[i][1] = vox4[i][1]; col[i][2] = vox4[i][2];
    }
}
void sculpt_vox_unpack16(const uint16_t *vox2, float *den, uint8_t (*col)[3]) {
    for (int z = 0; z < SN; z++)
    for (int y = 0; y < SN; y++)
    for (int x = 0; x < SN; x++) {
        int ox = (int)(x * 15.0f / (SN-1) + 0.5f);
        int oy = (int)(y * 15.0f / (SN-1) + 0.5f);
        int oz = (int)(z * 15.0f / (SN-1) + 0.5f);
        uint16_t v = vox2[(oz*16 + oy)*16 + ox];
        int i = vidx(x,y,z);
        den[i] = (v & 0x8000) ? 1.0f : 0.0f;
        col[i][0] = (uint8_t)(((v>>11) & 31) * 255 / 31);
        col[i][1] = (uint8_t)(((v>>5)  & 63) * 255 / 63);
        col[i][2] = (uint8_t)((v & 31) * 255 / 31);
    }
}
