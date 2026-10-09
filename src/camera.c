#include "camera.h"
#include "app.h"
#include "rk.h"
#include <math.h>
#include <string.h>

void cam_init(void) {
    A.camPos = v3_make(14, 12, 14);
    A.yaw = 0.785398f;   /* look toward origin */
    A.pitch = -0.45f;
    A.speed = 14.0f;
}

CamBasis cam_basis(void) {
    float cy = cosf(A.yaw), sy = sinf(A.yaw), cp = cosf(A.pitch), sp = sinf(A.pitch);
    CamBasis c;
    c.f = v3_make(-sy*cp, sp, -cy*cp);
    c.r = v3_make(cy, 0, -sy);
    c.u = v3_cross(c.f, c.r);          /* right-handed up */
    c.u = v3_scale(c.u, -1.0f);
    return c;
}

void cam_update_vp(void) {
    CamBasis c = cam_basis();
    m4 v, p;
    m4_look_at(v, A.camPos, v3_add(A.camPos, c.f), c.u);
    float aspect = A.fbh > 0 ? (float)A.fbw / A.fbh : 1.0f;
    m4_perspective_rz(p, 1.05f, aspect, 0.1f, 500.0f);
    m4_mul(A.viewProj, p, v);
}

void cam_ray(double mx, double my, v3 *ro, v3 *rd) {
    float ndcx = (float)(mx / A.fbw * 2.0 - 1.0);
    float ndcy = (float)(my / A.fbh * 2.0 - 1.0);
    CamBasis c = cam_basis();
    float t = tanf(1.05f * 0.5f);
    float aspect = (float)A.fbw / (float)A.fbh;
    *rd = v3_norm(v3_add(c.f, v3_add(v3_scale(c.r, ndcx*t*aspect),
                                   v3_scale(c.u, -ndcy*t))));
    *ro = A.camPos;
}

void cam_look(float dx, float dy) {
    A.yaw -= dx * 0.0042f;               /* mouse right -> look right */
    A.pitch = clampf(A.pitch - dy * 0.0042f, -1.55f, 1.55f);
}

void cam_pan(float dx, float dy) {
    CamBasis c = cam_basis();
    float s = A.speed * 0.12f;
    A.camPos = v3_add(A.camPos, v3_add(v3_scale(c.r, -dx*s*0.02f),
                                      v3_scale(c.u, dy*s*0.02f)));
}

void cam_speed_step(float dy) {
    A.speed = clampf(A.speed * (dy > 0 ? 1.15f : 0.87f), 1.0f, 120.0f);
    A.speedMsgT = glfwGetTime();
}

void cam_zoom_cursor(float dy, double mx, double my) {
    v3 ro, rd; cam_ray(mx, my, &ro, &rd);
    float t = -1.0f;
    if (rd.y < -1e-6f) t = -ro.y / rd.y;                 /* ground plane */
    v3 target;
    if (t > 0) target = v3_make(ro.x+rd.x*t, 0, ro.z+rd.z*t);
    else       target = v3_add(ro, v3_scale(rd, 30.0f));
    float d = v3_dist(A.camPos, target);
    float step = d * 0.18f * (dy > 0 ? 1.0f : -0.9f);    /* zoom in ~out symmetric-ish */
    if (dy > 0 && d - step < 1.5f) step = d - 1.5f;
    if (step != 0) {
        v3 dir = v3_norm(v3_sub(target, A.camPos));
        A.camPos = v3_add(A.camPos, v3_scale(dir, step));
    }
    A.speedMsgT = glfwGetTime();
}

void cam_focus_sel(void) {
    int n = 0;
    v3 c = v3_make(0,0,0);
    for (int i = 0; i < g_nb; i++) if (A.sel[i]) { c = v3_add(c, g_blocks[i].pos); n++; }
    if (!n) return;
    c = v3_scale(c, 1.0f/n);
    CamBasis b = cam_basis();
    A.camPos = v3_sub(c, v3_scale(b.f, 14.0f));
}

void cam_frame(float dt) {
    CamBasis c = cam_basis();
    int ctrl = glfwGetKey(A.win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    int lmb  = glfwGetMouseButton(A.win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    v3 acc = v3_make(0,0,0);
    if (glfwGetKey(A.win, GLFW_KEY_W) == GLFW_PRESS) acc = v3_add(acc, c.f);
    if (glfwGetKey(A.win, GLFW_KEY_S) == GLFW_PRESS) acc = v3_sub(acc, c.f);
    if (glfwGetKey(A.win, GLFW_KEY_D) == GLFW_PRESS) acc = v3_add(acc, c.r);
    if (glfwGetKey(A.win, GLFW_KEY_A) == GLFW_PRESS && !ctrl) acc = v3_sub(acc, c.r);
    if (glfwGetKey(A.win, GLFW_KEY_Q) == GLFW_PRESS) acc.y -= 1;
    if (glfwGetKey(A.win, GLFW_KEY_E) == GLFW_PRESS) acc.y += 1;
    if (glfwGetKey(A.win, GLFW_KEY_SPACE) == GLFW_PRESS) acc.y += 1;
    /* Shift = descend, but never mid-LMB-gesture (multi-select/marquee) */
    if (!lmb && glfwGetKey(A.win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) acc.y -= 1;
    /* velocity smoothing: accelerate toward input, damp otherwise */
    float target = A.speed;
    v3 want = v3_len(acc) > 0 ? v3_scale(v3_norm(acc), target) : v3_make(0,0,0);
    float k = 1.0f - expf(-10.0f * dt);        /* critically-damped-ish */
    A.camVel = v3_add(A.camVel, v3_scale(v3_sub(want, A.camVel), k));
    A.camPos = v3_add(A.camPos, v3_scale(A.camVel, dt));
}
