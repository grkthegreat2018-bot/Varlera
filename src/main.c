/* main.c - entry point, input dispatch, frame loop. */
#include <stdio.h>
#include <string.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app.h"
#include "camera.h"
#include "edit.h"
#include "panel.h"
#include "model.h"
#include "rk.h"
#include "world.h"
#include "math3d.h"

App A;

void app_toggle_mode(void) {
    if (A.app == APP_EDIT) {
        A.app = APP_PLAY;
        world_snapshot_play();
        A.drag = M_IDLE;
        A.armedAsset = -1;
    } else {
        A.app = APP_EDIT;
        world_restore_play();
    }
}

/* ---------------- glfw callbacks ---------------- */

static void on_cursor(GLFWwindow *w, double x, double y) {
    double dx = x - A.mx, dy = y - A.my;
    A.mx = x; A.my = y;
    if (A.drag == M_LOOK) cam_look((float)dx, (float)dy);
    if (A.drag == M_PAN)  cam_pan((float)dx, (float)dy);
    if (A.drag == M_MARQUEE) { A.marqX1 = (float)x; A.marqY1 = (float)y; }
    if (A.drag == M_CLICK || A.drag == M_MOVE) edit_lmb_motion();
}

static void on_mouse(GLFWwindow *w, int button, int action, int mods) {
    (void)w;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            A.overPanel = panel_over(A.mx, A.my);
            if (panel_press(A.mx, A.my)) return;
            if (A.app == APP_EDIT && panel_script_click(A.mx, A.my)) return;
            if (A.app == APP_EDIT) edit_lmb_press(mods);
        } else {
            A.pickDrag = 0;
            if (A.app == APP_EDIT) edit_lmb_release(mods);
            else A.drag = M_IDLE;
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            if (A.app == APP_EDIT && panel_rmb(A.mx, A.my)) return;
            A.drag = M_LOOK; A.rmbX = A.mx; A.rmbY = A.my;
        }
        else { edit_rmb_up((float)(A.mx - A.rmbX), (float)(A.my - A.rmbY)); A.drag = M_IDLE; }
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        A.drag = (action == GLFW_PRESS) ? M_PAN : M_IDLE;
    }
}

static int mods_ctrl(void) {
    return glfwGetKey(A.win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
           glfwGetKey(A.win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

static void on_scroll(GLFWwindow *w, double xoff, double yoff) {
    (void)w; (void)xoff;
    if (panel_over(A.mx, A.my)) { panel_scroll(yoff); return; }
    if (A.drag == M_MOVE || A.drag == M_CLICK) {  /* raise/lower while moving */
        A.dyMove += (float)yoff * 0.5f;
        if (A.drag == M_CLICK) { A.drag = M_MOVE; }
        return;
    }
    if (A.drag == M_PLACE || A.drag == M_ARMED) { /* rotate ghost w/ wheel */
        A.placeRot = (A.placeRot + (yoff > 0 ? 1 : 3)) & 3;
        return;
    }
    if (mods_ctrl()) cam_speed_step((float)yoff);
    else             cam_zoom_cursor((float)yoff, A.mx, A.my);
}

static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w; (void)sc;
    if (action == GLFW_RELEASE) return;
    if (key == GLFW_KEY_TAB && (action == GLFW_PRESS)) { app_toggle_mode(); return; }
    if (key == GLFW_KEY_ESCAPE && A.app == APP_PLAY) { app_toggle_mode(); return; }
    if (A.app == APP_EDIT) edit_key(key, mods);
}

static void on_resize(GLFWwindow *w, int width, int height) {
    (void)w;
    A.fbw = width; A.fbh = height;
}

/* ---------------- main ---------------- */

int main(void) {
    memset(&A, 0, sizeof A);
    if (!glfwInit()) { fputs("glfwInit failed\n", stderr); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    A.win = glfwCreateWindow(1280, 720, "Varlera", NULL, NULL);
    if (!A.win) { fputs("window failed\n", stderr); return 1; }
    glfwGetFramebufferSize(A.win, &A.fbw, &A.fbh);
    glfwSetInputMode(A.win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    glfwSetCursorPosCallback(A.win, on_cursor);
    glfwSetMouseButtonCallback(A.win, on_mouse);
    glfwSetScrollCallback(A.win, on_scroll);
    glfwSetKeyCallback(A.win, on_key);
    glfwSetFramebufferSizeCallback(A.win, on_resize);

    if (!rk_init(A.win)) { fputs("rk_init failed\n", stderr); return 1; }

    cam_init();
    panel_init();
    models_init();               /* reload saved custom models */
    world_load("world.bw");      /* auto-load last world if present */
    A.app = APP_EDIT;
    A.drag = M_IDLE;
    A.hoverBlock = A.hoverAxis = A.hoverAsset = -1;
    A.lastT = glfwGetTime();

    puts("Varlera controls:");
    puts("  RMB look | WASD+QE fly | space/shift up/down | wheel=zoom | MMB pan | F focus");
    puts("  LMB place/select/drag | shift multi | marquee | wheel=height while moving");
    puts("  R/T rotate | del delete | ctrl+Z/Y undo/redo | ctrl+D dup | ctrl+A all");
    puts("  ctrl+C/X/V clipboard | ctrl+B fuse | ctrl+U unfuse | ctrl+S/F5 save | F9 load");
    puts("  arrows nudge sel | TAB play/edit | esc cancel");

    while (!glfwWindowShouldClose(A.win)) {
        glfwPollEvents();
        double t = glfwGetTime();
        float dt = clampf((float)(t - A.lastT), 0.0f, 0.1f);
        A.lastT = t;
        if (A.fbw <= 0 || A.fbh <= 0) continue;

        cam_frame(dt);
        cam_update_vp();

        if (A.app == APP_PLAY) {
            /* poll script triggers -> per-block TRIG_* bits */
            static const int TK[N_TRIGS] = {
                0, GLFW_KEY_SPACE, GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S,
                GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_Q, 0 };
            uint16_t held = 1u << TRIG_ALWAYS;
            for (int i = 1; i < N_TRIGS; i++)
                if (TK[i] && glfwGetKey(A.win, TK[i]) == GLFW_PRESS) held |= 1u << i;
            if (glfwGetMouseButton(A.win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
                held |= 1u << TRIG_CLICK;
            A.trigHeld = held;
            world_physics(dt, held);
        }
        else if (A.app == APP_EDIT) { edit_update(); panel_drag_update(); }

        if (!rk_begin_frame()) continue;

        v3 eye = A.camPos;
        v3 focus = v3_add(A.camPos, v3_scale(cam_basis().f, 30.0f));
        rk_set_camera(A.viewProj, eye.e, focus.e);

        rk_draw_grid();
        edit_draw_world();
        panel_draw();          /* queues UI + icons, flushes internally */
        rk_ui_flush();

        rk_end_frame();
    }

    rk_shutdown();
    glfwDestroyWindow(A.win);
    glfwTerminate();
    return 0;
}
