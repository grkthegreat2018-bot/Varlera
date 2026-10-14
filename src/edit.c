/* edit.c - world-space editing interactions. */
#include "edit.h"
#include "app.h"
#include "camera.h"
#include "panel.h"
#include "model.h"
#include "toast.h"
#include "tex.h"
#include "rk.h"
#include "particles.h"
#include "physics.h"
#include "math3d.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#define GRID_SNAP 0.5f
#define SIZE_SNAP 0.5f

/* snap so the block's low face lands on the 0.5 lattice -> different-sized
   blocks pack flush regardless of their footprint */
static float snap_corner(float x, float w) {
    return snapf(x - w*0.5f, GRID_SNAP) + w*0.5f;
}

void sel_clear(void) { memset(A.sel, 0, sizeof A.sel); A.nsel = 0; }
int  sel_single(void) { return A.nsel == 1; }
int  sel_first(void) {
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) return i;
    return -1;
}
void delete_selected(void) {
    if (!A.nsel) return;
    world_push_undo();
    /* delete selected descending; moved-in blocks are always unselected */
    for (int i = g_nb-1; i >= 0; i--) if (A.sel[i]) world_delete(i);
    sel_clear();
}

/* ---------------- fuse / unfuse (custom models) ---------------- */

void fuse_selection(void) {
    if (A.nsel < 2 || A.nsel > MODEL_MAX_CHILDREN || g_nmodels >= RK_MAX_MODELS) return;
    static Block kids[MODEL_MAX_CHILDREN];
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    int n = 0;
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
        kids[n++] = g_blocks[i];
        v3 es = block_eff_size(&g_blocks[i]);
        for (int a = 0; a < 3; a++) {
            float l = g_blocks[i].pos.e[a] - es.e[a]*0.5f;
            float h = g_blocks[i].pos.e[a] + es.e[a]*0.5f;
            if (l < lo.e[a]) lo.e[a] = l;
            if (h > hi.e[a]) hi.e[a] = h;
        }
    }
    int mid = model_bake(kids, n);
    if (mid < 0) { puts("fuse failed (model limit)"); return; }
    rk_models_commit();                    /* upload the baked mesh */
    world_push_undo();
    for (int i = g_nb-1; i >= 0; i--) if (A.sel[i]) world_delete(i);
    v3 piv = v3_scale(v3_add(lo, hi), 0.5f);
    float white[3] = {0.85f, 0.85f, 0.85f};
    int ni = world_add(piv, g_models[mid].size, g_models[mid].mesh, 0, white);
    sel_clear();
    if (ni >= 0) { A.sel[ni] = 1; A.nsel = 1; }
    models_save(NULL);
    printf("fused %d blocks -> %s\n", n, g_models[mid].name);
    toast_tip(TIP_FUSE);
}

void unfuse_block(int i) {
    if (i < 0 || i >= g_nb) return;
    Block fb = g_blocks[i];                 /* copy: slot i gets swap-filled on delete */
    int mid = fb.shape - RK_MESH_BASE_MODEL;
    if (mid < 0 || mid >= g_nmodels) return;
    Model *m = &g_models[mid];
    world_push_undo();
    world_delete(i);
    sel_clear();
    int first = g_nb;
    if (m->vox) {
        /* sculpted topo model: explode into a micro-cube per vertex */
        v3 scl = v3_make(fb.size.x / m->size.x, fb.size.y / m->size.y,
                         fb.size.z / m->size.z);
        float cs = fmaxf(fmaxf(m->size.x * scl.x, m->size.y * scl.y),
                         m->size.z * scl.z) / 16.0f;
        if (cs < 0.05f) cs = 0.05f;
        v3 sz = v3_make(cs, cs, cs);
        for (int v = 0; v < m->nvert && g_nb < MAX_BLOCKS; v++) {
            float cc[3] = { m->mvcol[v][0]/255.0f, m->mvcol[v][1]/255.0f,
                            m->mvcol[v][2]/255.0f };
            v3 p = v3_make(m->mv[v][0]*scl.x, m->mv[v][1]*scl.y,
                           m->mv[v][2]*scl.z);
            for (int k = 0; k < (fb.rot & 3); k++) p = v3_make(p.z, p.y, -p.x);
            int ni = world_add(v3_add(fb.pos, p), sz, RK_MESH_CUBE, 0, cc);
            if (ni >= 0) g_blocks[ni].alpha = 1.0f;
        }
        for (int j = first; j < g_nb; j++) { A.sel[j] = 1; A.nsel++; }
        return;
    }
    for (int c = 0; c < m->nch; c++) {
        Block ch = m->ch[c];
        /* re-apply parent rotation to child offsets */
        v3 p = ch.pos;
        for (int k = 0; k < (fb.rot & 3); k++) p = v3_make(p.z, p.y, -p.x); /* matches shader rotY */
        int ni = world_add(v3_add(fb.pos, p), ch.size, ch.shape,
                           (ch.rot + fb.rot) & 3, ch.col);
        if (ni >= 0) {
            g_blocks[ni].tex = ch.tex; g_blocks[ni].texm = ch.texm;
            g_blocks[ni].alpha = ch.alpha; g_blocks[ni].flags = ch.flags;
            g_blocks[ni].trig = ch.trig; g_blocks[ni].act = ch.act;
            g_blocks[ni].actParam = ch.actParam; g_blocks[ni].tgt = ch.tgt;
            memcpy(g_blocks[ni].name, ch.name, sizeof ch.name);
        }
    }
    for (int j = first; j < g_nb; j++) { A.sel[j] = 1; A.nsel++; }
}

/* ---------------- picking ---------------- */

static int ray_ground(v3 ro, v3 rd, v3 *out) {
    if (rd.y > -1e-6f) return 0;
    float t = -ro.y / rd.y;
    *out = v3_make(ro.x + rd.x*t, 0, ro.z + rd.z*t);
    return 1;
}

/* pick_block moved to world.c (shared with physics.c play-mode drag) */
#define pick_block world_pick_block

/* 6 face handles: sign -1 = low face, +1 = high face */
static void handle_center(const Block *b, int axis, int sign, v3 *out) {
    v3 es = block_eff_size(b);
    *out = b->pos;
    out->e[axis] += sign * (es.e[axis]*0.5f + 0.55f);
}

static int pick_handle(v3 ro, v3 rd, int *axisOut, int *signOut) {
    int i = sel_first();
    if (i < 0) return 0;
    Block *b = &g_blocks[i];
    float bt = 1e30f; int ba = -1, bs = 0;
    for (int a = 0; a < 3; a++) for (int s = -1; s <= 1; s += 2) {
        v3 c; handle_center(b, a, s, &c);
        float h = 0.42f;
        v3 sr = v3_make((ro.x-c.x)/(h*2), (ro.y-c.y)/(h*2), (ro.z-c.z)/(h*2));
        v3 sd = v3_make(rd.x/(h*2), rd.y/(h*2), rd.z/(h*2));
        float t; int ax; float sg;
        if (ray_unit_box(sr, sd, &t, &ax, &sg) && t < bt) { bt = t; ba = a; bs = s; }
    }
    if (ba < 0) return 0;
    *axisOut = ba; *signOut = bs; return 1;
}

/* ---------------- ghost / place / move / resize ---------------- */

/* lift a candidate placement above every block it would intersect -
   stacks "onto", never phases "into" */
static void resolve_ghost_overlap(v3 *p, v3 gs) {
    for (int it = 0; it < 64; it++) {
        float top = -1e30f;
        for (int i = 0; i < g_nb; i++) {
            Block *b = &g_blocks[i];
            v3 es = block_eff_size(b);
            if (aabb_overlap_xz(*p, gs, b->pos, es, 1e-3f) &&
                fabsf(p->y - b->pos.y) < (gs.y + es.y)*0.5f - 1e-3f) {
                float bt = b->pos.y + es.y*0.5f;
                if (bt > top) top = bt;
            }
        }
        if (top < -1e29f) break;
        p->y = top + gs.y*0.5f;
    }
}

static void ghost_update(v3 ro, v3 rd) {
    const Asset *a = panel_asset(A.armedAsset);
    if (!a) return;
    A.ghostSize = a->size;          /* logical; shader applies rot */
    A.ghostShapeMesh = a->mesh;
    v3 gs = (A.placeRot & 1) ? v3_make(a->size.z, a->size.y, a->size.x) : a->size;
    int idx, axis; float t, sign;
    if (pick_block(ro, rd, &idx, &t, &axis, &sign) && axis >= 0) {
        /* attach to the hit face: snap to the target's own face lattice and
           clamp inside the face, so same-size blocks stack flush and edge
           grazes can't hang the ghost half-off (clipping neighbors) */
        Block *b = &g_blocks[idx];
        v3 es = block_eff_size(b);
        v3 hp = v3_make(ro.x+rd.x*t, ro.y+rd.y*t, ro.z+rd.z*t);
        v3 p = b->pos;
        for (int i = 0; i < 3; i++) {
            if (i == axis) continue;
            if (gs.e[i] >= es.e[i] - 1e-4f) { p.e[i] = b->pos.e[i]; continue; }
            float org = b->pos.e[i] - es.e[i]*0.5f;
            float c = org + snap_corner(hp.e[i] - org, gs.e[i]);
            p.e[i] = clampf(c, org + gs.e[i]*0.5f,
                            b->pos.e[i] + es.e[i]*0.5f - gs.e[i]*0.5f);
        }
        p.e[axis] = b->pos.e[axis] + sign * (es.e[axis]*0.5f + gs.e[axis]*0.5f);
        A.ghostPos = p;
    } else {
        v3 g;
        if (!ray_ground(ro, rd, &g)) return;
        A.ghostPos = v3_make(snap_corner(g.x, gs.x), gs.y*0.5f, snap_corner(g.z, gs.z));
    }
    resolve_ghost_overlap(&A.ghostPos, gs);
}

static void place_commit(void) {
    const Asset *a = panel_asset(A.armedAsset);
    if (!a) return;
    world_push_undo();
    int ni = world_add(A.ghostPos, a->size, a->mesh, A.placeRot, A.paintCol);
    if (ni >= 0) {
        g_blocks[ni].tex = (uint8_t)A.paintTex;
        g_blocks[ni].texm = (uint8_t)A.paintTexM;
        g_blocks[ni].alpha = A.paintAlpha;
        if (A.paintAlpha < 1.0f) g_blocks[ni].flags |= BF_GLASS;
        v3 at = g_blocks[ni].pos;
        at.y -= block_eff_size(&g_blocks[ni]).y * 0.5f;
        particles_burst(P_DUST, at, g_blocks[ni].col, 6);
        static const int catTip[4] = {TIP_PLACE_BASIC, TIP_PLACE_RAMP,
                                      TIP_PLACE_SPECIAL, TIP_PLACE_MODEL};
        int cat = panel_asset_cat(A.armedAsset);
        if (cat >= 0) toast_tip(catTip[cat]);
    }
}

static void begin_move(v3 hitPoint) {
    A.drag = M_MOVE;
    A.grabPoint = hitPoint;
    A.grabY = hitPoint.y;
    A.dyMove = 0;
    for (int i = 0; i < g_nb; i++) A.orig[i] = g_blocks[i].pos;
    world_push_undo();
}

static void update_move(v3 ro, v3 rd) {
    if (fabsf(rd.y) < 1e-6f) return;
    float t = (A.grabY - ro.y) / rd.y;
    if (t < 0) return;
    v3 hit = v3_make(ro.x+rd.x*t, A.grabY, ro.z+rd.z*t);
    float dx = snapf(hit.x-A.grabPoint.x, GRID_SNAP);
    float dz = snapf(hit.z-A.grabPoint.z, GRID_SNAP);
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
        g_blocks[i].pos.x = A.orig[i].x + dx;
        g_blocks[i].pos.z = A.orig[i].z + dz;
        g_blocks[i].pos.y = A.orig[i].y + A.dyMove;
        float half = block_eff_size(&g_blocks[i]).y*0.5f;
        if (g_blocks[i].pos.y < half) g_blocks[i].pos.y = half;
    }
}

static void update_resize(v3 ro, v3 rd) {
    int i = sel_first();
    if (i < 0) { A.drag = M_IDLE; return; }
    Block *b = &g_blocks[i];
    int a = A.resizeAxis, sg = A.resizeSign;
    v3 es0 = block_eff_size(&A.origBlock);
    /* ray-to-axis closest point (axis line through original center) */
    v3 u = v3_make(0,0,0); u.e[a] = 1;
    v3 w0 = v3_sub(ro, A.origBlock.pos);
    float b_ = v3_dot(rd, u), d_ = v3_dot(rd, w0), e_ = v3_dot(u, w0);
    float denom = 1.0f - b_*b_;
    if (fabsf(denom) < 1e-6f) return;
    float tl = (e_ - b_*d_) / denom;
    /* anchor = far face stays fixed; extent = dist from anchor to pointer */
    float anchor = A.origBlock.pos.e[a] - sg * es0.e[a] * 0.5f;
    float pc = A.origBlock.pos.e[a] + tl;              /* pointer coord on axis */
    float ext = clampf(snapf((pc - anchor) * sg, SIZE_SNAP), 0.5f, 32.0f);
    /* resize edits logical size; for odd rot x/z are swapped visually */
    int la = (b->rot & 1) && a != 1 ? (a == 0 ? 2 : 0) : a;
    b->size.e[la] = ext;
    b->pos.e[a] = anchor + sg * ext * 0.5f;
}

static void marquee_apply(int additive) {
    if (!additive) sel_clear();
    float x0 = fminf(A.marqX0, A.marqX1), x1 = fmaxf(A.marqX0, A.marqX1);
    float y0 = fminf(A.marqY0, A.marqY1), y1 = fmaxf(A.marqY0, A.marqY1);
    for (int i = 0; i < g_nb; i++) {
        if (g_blocks[i].flags & BF_LOCKED) continue;
        v3 clip; float w;
        m4_mul_v4(&clip, &w, A.viewProj, g_blocks[i].pos);
        if (w <= 0.001f) continue;
        float px = (clip.x/w*0.5f+0.5f)*A.fbw;
        float py = (clip.y/w*0.5f+0.5f)*A.fbh;
        if (px>=x0 && px<=x1 && py>=y0 && py<=y1 && !A.sel[i]) { A.sel[i]=1; A.nsel++; }
    }
}

/* ---------------- input entry points ---------------- */

static void clone_selection(void) { /* duplicate selected blocks in place, select copies */
    if (!A.nsel) return;
    world_push_undo();
    int src[MAX_BLOCKS], n = 0;
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) src[n++] = i;
    sel_clear();
    for (int i = 0; i < n; i++) {
        Block b = g_blocks[src[i]];
        int ni = world_addb(&b);
        if (ni >= 0) {
            g_blocks[ni].id = g_nextid++;       /* clones are new instances */
            g_blocks[ni].flags &= ~BF_LOCKED;
            A.sel[ni] = 1; A.nsel++;
        }
    }
}

static void copy_clip(void) {
    A.nClip = 0;
    if (!A.nsel) return;
    /* reference point: center of the selection's footprint (x/z), bottom of
       the lowest block (y). Using the AABB - not the centroid of centers -
       keeps stored offsets formation-exact so pastes can't collapse blocks
       onto each other or drop multi-height stacks through the ground. */
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
        v3 es = block_eff_size(&g_blocks[i]);
        for (int a = 0; a < 3; a++) {
            float l = g_blocks[i].pos.e[a] - es.e[a] * 0.5f;
            float h = g_blocks[i].pos.e[a] + es.e[a] * 0.5f;
            if (l < lo.e[a]) lo.e[a] = l;
            if (h > hi.e[a]) hi.e[a] = h;
        }
    }
    v3 ref = v3_make((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
    for (int i = 0; i < g_nb && A.nClip < MAX_BLOCKS; i++) if (A.sel[i]) {
        A.clip[A.nClip] = g_blocks[i];
        A.clip[A.nClip].pos = v3_sub(g_blocks[i].pos, ref);
        A.nClip++;
    }
}

static void paste_clip(v3 at) {
    if (!A.nClip) return;
    world_push_undo();
    sel_clear();
    /* lift the whole formation above any existing blocks it would overlap -
       relative offsets stay exact, nothing embeds into what's already there */
    for (int pass = 0; pass < 8; pass++) {
        float lift = 0;
        for (int i = 0; i < A.nClip; i++) {
            v3 p = v3_add(A.clip[i].pos, at);
            v3 es = block_eff_size(&A.clip[i]);
            for (int j = 0; j < g_nb; j++) {
                v3 bs = block_eff_size(&g_blocks[j]);
                if (aabb_overlap_xz(p, es, g_blocks[j].pos, bs, 1e-3f) &&
                    p.y-es.y*0.5f < g_blocks[j].pos.y+bs.y*0.5f-1e-3f &&
                    p.y+es.y*0.5f > g_blocks[j].pos.y-bs.y*0.5f+1e-3f) {
                    float need = g_blocks[j].pos.y+bs.y*0.5f - (p.y-es.y*0.5f);
                    if (need > lift) lift = need;
                }
            }
        }
        if (lift <= 0) break;
        at.y += lift;
    }
    for (int i = 0; i < A.nClip; i++) {
        Block b = A.clip[i];
        /* `at` is already grid-snapped by the caller; adding the exact
           stored offset keeps the formation coherent - no per-block snap,
           no per-block ground clamp (the ref bottom lands on `at.y`) */
        b.pos = v3_add(b.pos, at);
        int ni = world_addb(&b);
        if (ni >= 0) {
            g_blocks[ni].id = g_nextid++;
            g_blocks[ni].flags &= ~BF_LOCKED;
            A.sel[ni] = 1; A.nsel++;
        }
    }
}

void edit_lmb_press(int mods) {
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    if (A.drag == M_ARMED) {            /* armed placement click: place and
                                           stay armed for the next one */
        ghost_update(ro, rd);
        place_commit();
        return;
    }
    int axis, hsign;
    if (sel_single() && pick_handle(ro, rd, &axis, &hsign)) {
        A.drag = M_RESIZE; A.resizeAxis = axis; A.resizeSign = hsign;
        A.origBlock = g_blocks[sel_first()];
        world_push_undo();
        return;
    }
    int idx, ax; float t, sg;
    if (pick_block(ro, rd, &idx, &t, &ax, &sg)) {
        if (mods & GLFW_MOD_SHIFT) {
            if (A.sel[idx]) { A.sel[idx]=0; A.nsel--; }
            else { A.sel[idx]=1; A.nsel++; }
            return;
        }
        /* double-click: select all blocks sharing this color */
        double now = glfwGetTime();
        if (idx == A.lastClickBlock && now - A.lastClickT < 0.35) {
            sel_clear();
            const float *c = g_blocks[idx].col;
            for (int i = 0; i < g_nb; i++)
                if (memcmp(g_blocks[i].col, c, sizeof(float)*3) == 0) { A.sel[i]=1; A.nsel++; }
            A.lastClickT = 0; A.lastClickBlock = -1;
            return;
        }
        A.lastClickT = now; A.lastClickBlock = idx;
        if (mods & GLFW_MOD_ALT) {      /* clone-drag: copy selection, move copies */
            if (!A.sel[idx]) { sel_clear(); A.sel[idx]=1; A.nsel=1; }
            clone_selection();
        } else if (!A.sel[idx]) { sel_clear(); A.sel[idx]=1; A.nsel=1; }
        A.drag = M_CLICK;               /* move only after drag threshold */
        A.pressX = (float)A.mx; A.pressY = (float)A.my;
        A.pressHit = v3_make(ro.x+rd.x*t, ro.y+rd.y*t, ro.z+rd.z*t);
        A.pressBlock = idx;
        return;
    }
    if (!(mods & GLFW_MOD_SHIFT)) sel_clear();
    A.drag = M_MARQUEE;
    A.marqX0 = A.marqX1 = (float)A.mx; A.marqY0 = A.marqY1 = (float)A.my;
}

/* RMB release with no drag = eyedropper (pick block color);
   while an asset is armed a short RMB click cancels placement instead */
void edit_rmb_up(float totalDx, float totalDy) {
    if (fabsf(totalDx) + fabsf(totalDy) > 8.0f) return;
    if (A.armedAsset >= 0) { A.armedAsset = -1; A.drag = M_IDLE; return; }
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    int idx, ax; float t, sg;
    if (pick_block(ro, rd, &idx, &t, &ax, &sg)) {
        memcpy(A.paintCol, g_blocks[idx].col, sizeof(float)*3);
        A.paintTex = g_blocks[idx].tex;    /* eyedropper picks texture too */
        A.curColor = -1;
    }
}

void edit_lmb_release(int mods) {
    switch (A.drag) {
    case M_PLACE: {
        if (panel_over(A.mx, A.my)) {   /* released back on panel -> arm asset */
            A.drag = M_ARMED;
            return;
        }
        v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
        ghost_update(ro, rd);
        place_commit();
        break; }
    case M_MARQUEE:
        marquee_apply(mods & GLFW_MOD_SHIFT);
        break;
    default: break;
    }
    if (A.drag != M_ARMED) A.drag = M_IDLE;
}

void edit_lmb_motion(void) {            /* click -> move once past threshold */
    if (A.drag == M_CLICK) {
        float dx = (float)A.mx - A.pressX, dy = (float)A.my - A.pressY;
        if (dx*dx + dy*dy > 36.0f) begin_move(A.pressHit);
    }
}

void edit_quicksave(void) {
    worlds_ensure();
    /* async when possible; a busy autosave falls back to the sync path */
    if (world_save_async(worlds_file(A.worldName, "bw"), A.worldName) < 0)
        world_save(worlds_file(A.worldName, "bw"), A.worldName);
    tex_save("textures.bw");
    rk_screenshot(worlds_file(A.worldName, "bmp"));   /* world thumbnail */
    fprintf(stderr, "saved -> %s\n", worlds_file(A.worldName, "bw"));
}

void edit_key(int key, int mods) {
    int ctrl = mods & GLFW_MOD_CONTROL;
    switch (key) {
    case GLFW_KEY_DELETE: case GLFW_KEY_BACKSPACE: delete_selected(); break;
    case GLFW_KEY_ESCAPE:
        if (A.drag == M_MOVE)
            for (int i = 0; i < g_nb; i++) if (A.sel[i]) g_blocks[i].pos = A.orig[i];
        A.drag = M_IDLE; A.armedAsset = -1; sel_clear();
        break;
    case GLFW_KEY_R:
        if (A.drag == M_PLACE || A.drag == M_ARMED) {
            A.placeRot = (A.placeRot + 1) & 3;
        } else if (A.nsel) {
            world_push_undo();
            for (int i = 0; i < g_nb; i++) if (A.sel[i])
                g_blocks[i].rot = (g_blocks[i].rot + 1) & 3;
        }
        break;
    case GLFW_KEY_T:                     /* rotate counter-clockwise */
        if (A.drag == M_PLACE || A.drag == M_ARMED) {
            A.placeRot = (A.placeRot + 3) & 3;
        } else if (A.nsel) {
            world_push_undo();
            for (int i = 0; i < g_nb; i++) if (A.sel[i])
                g_blocks[i].rot = (g_blocks[i].rot + 3) & 3;
        }
        break;
    case GLFW_KEY_PAGE_UP: case GLFW_KEY_PAGE_DOWN:
        if (A.nsel) {
            world_push_undo();
            float d = key == GLFW_KEY_PAGE_UP ? SIZE_SNAP : -SIZE_SNAP;
            for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
                g_blocks[i].pos.y += d;
                float half = block_eff_size(&g_blocks[i]).y*0.5f;
                if (g_blocks[i].pos.y < half) g_blocks[i].pos.y = half;
            }
        }
        break;
    case GLFW_KEY_C: if (ctrl) copy_clip(); break;
    case GLFW_KEY_X: if (ctrl) { copy_clip(); delete_selected(); } break;
    case GLFW_KEY_V:
        if (ctrl && A.nClip) {
            v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
            v3 g; if (!ray_ground(ro, rd, &g)) g = v3_make(0,0,0);
            paste_clip(v3_make(snapf(g.x,GRID_SNAP), g.y, snapf(g.z,GRID_SNAP)));
        }
        break;
    case GLFW_KEY_D:
        if (ctrl && A.nsel) {
            clone_selection();
            for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
                g_blocks[i].pos.x += 1; g_blocks[i].pos.z += 1;
            }
        }
        break;
    case GLFW_KEY_A:
        if (ctrl) { memset(A.sel, 1, g_nb); A.nsel = g_nb;
            for (int i = 0; i < g_nb; i++) if (g_blocks[i].flags & BF_LOCKED) { A.sel[i]=0; A.nsel--; } }
        break;
    case GLFW_KEY_B:
        if (ctrl) fuse_selection();
        break;
    case GLFW_KEY_U:
        if (ctrl && sel_single()) unfuse_block(sel_first());
        break;
    case GLFW_KEY_Z: if (ctrl && world_undo()) { sel_clear(); } break;
    case GLFW_KEY_Y: if (ctrl && world_redo()) { sel_clear(); } break;
    case GLFW_KEY_S: if (ctrl) edit_quicksave(); break;
    case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT:
    case GLFW_KEY_UP:   case GLFW_KEY_DOWN:
        /* nudge selection 0.5 in view-relative horizontal direction */
        if (A.nsel) {
            CamBasis cb = cam_basis();
            v3 fwd = v3_norm(v3_make(cb.f.x, 0, cb.f.z));
            v3 rgt = v3_norm(v3_make(cb.r.x, 0, cb.r.z));
            v3 d = key == GLFW_KEY_UP    ? fwd : key == GLFW_KEY_DOWN  ? v3_scale(fwd,-1.0f)
                 : key == GLFW_KEY_RIGHT ? rgt : v3_scale(rgt,-1.0f);
            world_push_undo();
            for (int i = 0; i < g_nb; i++) if (A.sel[i])
                g_blocks[i].pos = v3_add(g_blocks[i].pos, v3_scale(d, SIZE_SNAP));
        }
        break;
    case GLFW_KEY_F5: edit_quicksave(); break;
    case GLFW_KEY_F9:
        if (world_load(worlds_file(A.worldName, "bw"), A.worldName) ||
            world_load("world.bw", A.worldName))
            puts("loaded world"); else puts("no saved world");
        sel_clear();
        break;
    case GLFW_KEY_F: cam_focus_sel(); break;
    default: break;
    }
}

void edit_update(void) {
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    if (A.drag == M_PLACE || A.drag == M_ARMED) ghost_update(ro, rd);
    if (A.drag == M_MOVE) update_move(ro, rd);
    if (A.drag == M_RESIZE) update_resize(ro, rd);
    if (A.drag == M_IDLE) {
        int ax, sg;
        A.hoverAxis = (sel_single() && pick_handle(ro, rd, &ax, &sg)) ? ax*2 + (sg>0) : -1;
        A.hoverBlock = -1;
        if (A.hoverAxis < 0) {
            int idx, a2; float t, sg2;
            if (pick_block(ro, rd, &idx, &t, &a2, &sg2)) A.hoverBlock = idx;
        }
    }
}

/* ---------------- draw ---------------- */

static uint32_t block_flag(const Block *b) {
    uint32_t f = ((uint32_t)b->rot << 1) | RKIF_TEX(b->tex) |
                 RKIF_TEXM(b->texm & 3u) | RKIF_MAT(b->texm >> 3);
    if (b->texm & TEXM_OVR) f |= RKIF_TEXOVR;
    if (b->shape >= RK_MESH_BASE_MODEL) f |= RKIF_VCOL;
    if (A.app == APP_PLAY) {
        int on = (A.trigHeld >> b->trig) & 1;
        if ((b->flags & BF_SPIN) || (b->act == ACT_SPIN && on)) f |= RKIF_SPIN;
        if (b->act == ACT_GLOW && on) f |= RKIF_UNLIT;
    }
    return f;
}

void edit_draw_world(void) {
    /* instanced draw: one call per mesh type (built-ins + baked models).
     * translucent blocks (alpha<1 or BF_GLASS) go through rk_ghost last,
     * sorted far->near for roughly correct blending. */
    static RkInst insts[RK_MESH_BASE_MODEL + RK_MAX_MODELS][MAX_BLOCKS];
    static RkInst cinsts[RK_MESH_BASE_MODEL + RK_MAX_MODELS][MAX_BLOCKS];
    static uint32_t cnt[RK_MESH_BASE_MODEL + RK_MAX_MODELS];
    static uint32_t ccnt[RK_MESH_BASE_MODEL + RK_MAX_MODELS];
    memset(cnt, 0, sizeof cnt);
    memset(ccnt, 0, sizeof ccnt);
    float pl[6][4]; frustum_planes(A.viewProj, pl);
    static int gi[MAX_BLOCKS];
    int gn = 0;
    for (int i = 0; i < g_nb; i++) {
        Block *b = &g_blocks[i];
        if (b->flags & BF_MERGED) continue;   /* drawn via merged cluster */
        if (A.app == APP_PLAY && world_player_taken(i)) continue;
        v3 es = block_eff_size(b);
        v3 half = v3_scale(es, 0.5f);
        int vis = aabb_in_frustum(pl, b->pos, half);
        float al = (b->flags & BF_GLASS) ? fminf(b->alpha, 0.6f) : b->alpha;
        if (A.modal == 2) al *= 0.18f;      /* sculpt modal: world recedes */
        if (al < 0.999f) { if (vis) gi[gn++] = i; continue; }
        RkInst in;
        memcpy(in.pos, b->pos.e, 12);
        memcpy(in.scale, es.e, 12);
        in.color = rk_rgba(b->col[0], b->col[1], b->col[2], 1.0f);
        in.flags = block_flag(b);
        /* off-camera opaque blocks still feed the shadow pass if they can
           cast into the shadowed volume; otherwise dropped entirely */
        if (vis) insts[b->shape][cnt[b->shape]++] = in;
        else if (rk_can_shadow_cast(b->pos, half))
            cinsts[b->shape][ccnt[b->shape]++] = in;
    }
    for (int m = 0; m < RK_MESH_BASE_MODEL + RK_MAX_MODELS; m++) {
        if (cnt[m]) rk_world(m, insts[m], cnt[m]);
        if (ccnt[m]) rk_caster(m, cinsts[m], ccnt[m]);
    }
    world_merged_draw();                    /* play-mode cluster meshes */

    /* translucent: sort by view depth, emit runs of same mesh */
    for (int i = 1; i < gn; i++) {            /* insertion sort, far first */
        int k = gi[i], j = i - 1;
        float dk = v3_dot(v3_sub(g_blocks[k].pos, A.camPos), cam_basis().f);
        while (j >= 0 && v3_dot(v3_sub(g_blocks[gi[j]].pos, A.camPos), cam_basis().f) < dk) {
            gi[j+1] = gi[j]; j--;
        }
        gi[j+1] = k;
    }
    int gi0 = 0;
    while (gi0 < gn) {
        int sh = g_blocks[gi[gi0]].shape, gi1 = gi0;
        static RkInst tinst[MAX_BLOCKS];
        uint32_t tn = 0;
        while (gi1 < gn && g_blocks[gi[gi1]].shape == sh) {
            Block *b = &g_blocks[gi[gi1]];
            float al = (b->flags & BF_GLASS) ? fminf(b->alpha, 0.6f) : b->alpha;
            if (A.modal == 2) al *= 0.18f;
            v3 es = block_eff_size(b);
            memcpy(tinst[tn].pos, b->pos.e, 12);
            memcpy(tinst[tn].scale, es.e, 12);
            tinst[tn].color = rk_rgba(b->col[0], b->col[1], b->col[2], al);
            tinst[tn].flags = block_flag(b);
            tn++; gi1++;
        }
        rk_ghost(sh, tinst, tn);
        gi0 = gi1;
    }

    /* selection boxes + hover */
    static RkInst lines[MAX_BLOCKS + 8];
    int ln = 0;
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) {
        Block *b = &g_blocks[i];
        v3 es = v3_scale(block_eff_size(b), 1.06f);
        lines[ln].flags = RKIF_UNLIT | RKIF_NOFOG;
        memcpy(lines[ln].pos, b->pos.e, 12);
        memcpy(lines[ln].scale, es.e, 12);
        lines[ln].color = rk_rgba(0.30f, 0.95f, 0.45f, 0.9f);
        ln++;
    }
    if (A.hoverBlock >= 0 && !A.sel[A.hoverBlock]) {
        Block *b = &g_blocks[A.hoverBlock];
        v3 es = v3_scale(block_eff_size(b), 1.05f);
        memcpy(lines[ln].pos, b->pos.e, 12);
        memcpy(lines[ln].scale, es.e, 12);
        lines[ln].color = rk_rgba(1,1,1,0.55f);
        lines[ln].flags = RKIF_UNLIT | RKIF_NOFOG;
        ln++;
    }
    if (A.drag == M_MARQUEE) { /* nothing in 3d */ }
    if (ln) rk_lines(lines, (uint32_t)ln);

    /* resize handles: 6 face handles (single selection) */
    if (sel_single() && A.drag != M_PLACE && A.drag != M_ARMED) {
        int i = sel_first(); Block *b = &g_blocks[i];
        const float hc[3][3] = {{0.95f,0.28f,0.28f},{0.30f,0.9f,0.30f},{0.30f,0.55f,0.95f}};
        RkInst h[6];
        for (int a = 0; a < 3; a++) for (int s = -1; s <= 1; s += 2) {
            int hi2 = a*2 + (s > 0);
            v3 c; handle_center(b, a, s, &c);
            float sc = (A.hoverAxis == hi2) ? 0.72f : 0.5f;
            memcpy(h[hi2].pos, c.e, 12);
            h[hi2].scale[0]=h[hi2].scale[1]=h[hi2].scale[2]=sc;
            h[hi2].color = rk_rgba(hc[a][0],hc[a][1],hc[a][2],1);
            h[hi2].flags = RKIF_UNLIT | RKIF_NOFOG;
        }
        rk_overlay(RK_MESH_CUBE, h, 6);
    }

    /* ghost */
    if ((A.drag == M_PLACE || A.drag == M_ARMED) && A.ghostSize.x > 0) {
        RkInst g;
        memcpy(g.pos, A.ghostPos.e, 12);
        memcpy(g.scale, A.ghostSize.e, 12);
        g.color = rk_rgba(A.paintCol[0],A.paintCol[1],A.paintCol[2], 0.55f);
        g.flags = (uint32_t)(A.placeRot << 1) | RKIF_TEX(A.paintTex) |
                  RKIF_TEXM(A.paintTexM & 3u);
        if (A.paintTexM & TEXM_OVR) g.flags |= RKIF_TEXOVR;
        if (A.ghostShapeMesh >= RK_MESH_BASE_MODEL) g.flags |= RKIF_VCOL;
        rk_ghost(A.ghostShapeMesh, &g, 1);
        RkInst l = g;
        v3 es = v3_scale(A.ghostSize, 1.06f);
        memcpy(l.scale, es.e, 12);
        l.color = rk_rgba(1,1,1,0.8f);
        l.flags = RKIF_UNLIT | RKIF_NOFOG;
        rk_lines(&l, 1);
    }
}
