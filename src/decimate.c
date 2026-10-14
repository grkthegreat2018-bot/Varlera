/* decimate.c - quadric-error edge collapse (Garland-Heckbert).
 * Sculpted models are small (<=512v/1024f), so a naive full edge rescan
 * per collapse is fine: O(collapses * edges) ~ a few hundred ops.
 * Boundary edges get a strong plane weight so silhouettes survive. */
#include "decimate.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

typedef struct { float m[10]; } Quad;   /* symmetric 4x4, upper triangle */

static void quad_plane(Quad *q, float a, float b, float c, float d) {
    q->m[0]+=a*a; q->m[1]+=a*b; q->m[2]+=a*c; q->m[3]+=a*d;
    q->m[4]+=b*b; q->m[5]+=b*c; q->m[6]+=b*d;
    q->m[7]+=c*c; q->m[8]+=c*d;
    q->m[9]+=d*d;
}
static void quad_add(Quad *a, const Quad *b) {
    for (int i = 0; i < 10; i++) a->m[i] += b->m[i];
}
static float quad_cost(const Quad *q, float x, float y, float z) {
    float xx=x*x, yy=y*y, zz=z*z;
    return q->m[0]*xx + 2*q->m[1]*x*y + 2*q->m[2]*x*z + 2*q->m[3]*x
         + q->m[4]*yy + 2*q->m[5]*y*z + 2*q->m[6]*y
         + q->m[7]*zz + 2*q->m[8]*z + q->m[9];
}

/* optimal point for Q: solve the 3x3, else best of {v1,v2,mid} */
static void quad_min(const Quad *q, const float a[3], const float b[3],
                     float out[3]) {
    float A[3][3] = {
        {q->m[0], q->m[1], q->m[2]},
        {q->m[1], q->m[4], q->m[5]},
        {q->m[2], q->m[5], q->m[7]},
    };
    float bv[3] = {-q->m[3], -q->m[6], -q->m[8]};
    /* gaussian elimination with partial pivot */
    float M[3][4];
    for (int i = 0; i < 3; i++) {
        memcpy(M[i], A[i], 12); M[i][3] = bv[i];
    }
    int ok = 1;
    for (int c = 0; c < 3 && ok; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (fabsf(M[r][c]) > fabsf(M[p][c])) p = r;
        if (fabsf(M[p][c]) < 1e-9f) { ok = 0; break; }
        for (int k = c; k < 4; k++) {
            float t = M[c][k]; M[c][k] = M[p][k]; M[p][k] = t;
        }
        for (int r = c + 1; r < 3; r++) {
            float f = M[r][c] / M[c][c];
            for (int k = c; k < 4; k++) M[r][k] -= f * M[c][k];
        }
    }
    if (ok) {
        for (int r = 2; r >= 0; r--) {
            float s = M[r][3];
            for (int k = r + 1; k < 3; k++) s -= M[r][k] * out[k];
            out[r] = s / M[r][r];
        }
        return;
    }
    /* fallback: cheapest of endpoints / midpoint */
    float mid[3] = {(a[0]+b[0])*0.5f, (a[1]+b[1])*0.5f, (a[2]+b[2])*0.5f};
    float ca = quad_cost(q, a[0],a[1],a[2]),
          cb = quad_cost(q, b[0],b[1],b[2]),
          cm = quad_cost(q, mid[0],mid[1],mid[2]);
    const float *bp = (ca <= cb && ca <= cm) ? a : (cb <= cm ? b : mid);
    memcpy(out, bp, 12);
}

int mesh_decimate(float (*vx)[3], uint8_t (*col)[3], int8_t *tex,
                  uint8_t *lock, uint16_t (*fc)[3], int *nvio, int nfc,
                  int targetF) {
    int nv = *nvio;
    if (nv < 4 || nfc <= targetF || targetF < 4) return nfc;
    Quad *Q = calloc((size_t)nv, sizeof *Q);
    int *dead = calloc((size_t)nv, sizeof(int));
    if (!Q || !dead) { free(Q); free(dead); return nfc; }

    /* face quadrics */
    for (int f = 0; f < nfc; f++) {
        float *a = vx[fc[f][0]], *b = vx[fc[f][1]], *c = vx[fc[f][2]];
        float ux=b[0]-a[0], uy=b[1]-a[1], uz=b[2]-a[2];
        float wx=c[0]-a[0], wy=c[1]-a[1], wz=c[2]-a[2];
        float nx=uy*wz-uz*wy, ny=uz*wx-ux*wz, nz=ux*wy-uy*wx;
        float l = sqrtf(nx*nx+ny*ny+nz*nz);
        if (l < 1e-9f) continue;
        nx/=l; ny/=l; nz/=l;
        float d = -(nx*a[0] + ny*a[1] + nz*a[2]);
        Quad qp = {0}; quad_plane(&qp, nx, ny, nz, d);
        for (int k = 0; k < 3; k++) quad_add(&Q[fc[f][k]], &qp);
    }
    /* boundary penalty: edge on 1 face -> perpendicular plane x50 */
    for (int f = 0; f < nfc; f++) {
        for (int e = 0; e < 3; e++) {
            int i0 = fc[f][e], i1 = fc[f][(e+1)%3];
            int cnt = 0;
            for (int g = 0; g < nfc; g++)
                for (int h = 0; h < 3; h++) {
                    int j0 = fc[g][h], j1 = fc[g][(h+1)%3];
                    if ((j0==i0&&j1==i1)||(j0==i1&&j1==i0)) cnt++;
                }
            if (cnt != 1) continue;
            float *a = vx[i0], *b = vx[i1];
            float ex=b[0]-a[0], ey=b[1]-a[1], ez=b[2]-a[2];
            /* face normal */
            int o = fc[f][(e+2)%3];
            float *c = vx[o];
            float ux=c[0]-a[0], uy=c[1]-a[1], uz=c[2]-a[2];
            float nx=ey*uz-ez*uy, ny=ez*ux-ex*uz, nz=ex*uy-ey*ux;
            float l = sqrtf(nx*nx+ny*ny+nz*nz);
            if (l < 1e-9f) continue;
            nx/=l; ny/=l; nz/=l;
            float d = -(nx*a[0]+ny*a[1]+nz*a[2]);
            Quad qp = {0};
            float w = 50.0f;
            qp.m[0]=nx*nx*w; qp.m[1]=nx*ny*w; qp.m[2]=nx*nz*w; qp.m[3]=nx*d*w;
            qp.m[4]=ny*ny*w; qp.m[5]=ny*nz*w; qp.m[6]=ny*d*w;
            qp.m[7]=nz*nz*w; qp.m[8]=nz*d*w; qp.m[9]=d*d*w;
            quad_add(&Q[i0], &qp); quad_add(&Q[i1], &qp);
        }
    }

    /* collapse loop: cheapest edge each round */
    while (nfc > targetF) {
        float best = 1e30f; int be0 = -1, be1 = -1; float bp[3] = {0};
        for (int f = 0; f < nfc; f++) {
            for (int e = 0; e < 3; e++) {
                int i0 = fc[f][e], i1 = fc[f][(e+1)%3];
                if (dead[i0] || dead[i1] || i0 == i1) continue;
                Quad qs = Q[i0]; quad_add(&qs, &Q[i1]);
                float p[3];
                quad_min(&qs, vx[i0], vx[i1], p);
                float c = quad_cost(&qs, p[0], p[1], p[2]);
                if (c < best) { best = c; be0 = i0; be1 = i1;
                                memcpy(bp, p, 12); }
            }
        }
        if (be0 < 0) break;
        /* merge i1 into i0 */
        memcpy(vx[be0], bp, 12);
        quad_add(&Q[be0], &Q[be1]);
        Q[be1].m[0] = 0;
        dead[be1] = 1;
        for (int f = 0; f < nfc; f++)
            for (int k = 0; k < 3; k++)
                if (fc[f][k] == be1) fc[f][k] = (uint16_t)be0;
        /* drop degenerate faces */
        int w2 = 0;
        for (int f = 0; f < nfc; f++)
            if (fc[f][0] != fc[f][1] && fc[f][1] != fc[f][2] &&
                fc[f][0] != fc[f][2]) {
                if (w2 != f) memcpy(fc[w2], fc[f], sizeof fc[0]);
                w2++;
            }
        nfc = w2;
        (void)best;
    }
    /* compact dead verts, remap faces, carry attributes */
    {
        int *remap = malloc((size_t)nv * sizeof(int));
        int wv = 0;
        for (int i = 0; i < nv; i++) {
            if (dead[i]) { remap[i] = -1; continue; }
            remap[i] = wv;
            if (wv != i) {
                memcpy(vx[wv], vx[i], 12);
                if (col) memcpy(col[wv], col[i], 3);
                if (tex) tex[wv] = tex[i];
                if (lock) lock[wv] = lock[i];
            }
            wv++;
        }
        for (int f = 0; f < nfc; f++)
            for (int k = 0; k < 3; k++) fc[f][k] = (uint16_t)remap[fc[f][k]];
        *nvio = wv;
        free(remap);
    }
    free(Q); free(dead);
    return nfc;
}
