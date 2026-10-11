/* main.c - entry point, input dispatch, frame loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app.h"
#include "camera.h"
#include "edit.h"
#include "panel.h"
#include "model.h"
#include "art.h"
#include "sculpt.h"
#include "tex.h"
#include "rk.h"
#include "prof.h"
#include "toast.h"
#include "settings.h"
#include "world.h"
#include "math3d.h"

App A;

void app_toggle_mode(void) {
    if (A.app == APP_EDIT) {
        A.app = APP_PLAY;
        world_snapshot_play();
        A.drag = M_IDLE;
        A.armedAsset = -1;
        toast_tip(TIP_PLAY);
    } else {
        A.app = APP_EDIT;
        world_restore_play();
    }
}

/* ---------------- glfw callbacks ---------------- */

static void on_char(GLFWwindow *w, unsigned int cp) {
    (void)w;
    if (A.modal == 2) { sculpt_char(cp); return; }
    if (!A.textMode || cp < 32 || cp >= 127) return;
    if (A.textMode == 1) {
        int n = (int)strlen(A.search);
        if (n < 32) { A.search[n] = (char)cp; A.search[n+1] = 0; }
    } else if (A.textMode == 2) {
        if (A.cmdLen < 95) { A.cmdBuf[A.cmdLen++] = (char)cp; A.cmdBuf[A.cmdLen] = 0; }
    } else if (A.textMode == 3) {
        int n = (int)strlen(A.renBuf);
        if (n < 11) { A.renBuf[n] = (char)cp; A.renBuf[n+1] = 0; }
    }
}

static void toggle_fullscreen(void) {
    if (!A.fullscreen) {
        glfwGetWindowPos(A.win, &A.winX, &A.winY);
        glfwGetWindowSize(A.win, &A.winW, &A.winH);
        GLFWmonitor *m = glfwGetPrimaryMonitor();
        const GLFWvidmode *vm = glfwGetVideoMode(m);
        glfwSetWindowMonitor(A.win, m, 0, 0, vm->width, vm->height, GLFW_DONT_CARE);
        A.fullscreen = 1;
    } else {
        glfwSetWindowMonitor(A.win, NULL, A.winX, A.winY, A.winW, A.winH, GLFW_DONT_CARE);
        A.fullscreen = 0;
    }
    settings_save();
}

static void on_cursor(GLFWwindow *w, double x, double y) {
    double dx = x - A.mx, dy = y - A.my;
    (void)w;
    A.mx = x; A.my = y;
    if (A.modal == 1) { art_motion(); return; }
    if (A.modal == 2) {
        sculpt_move((int)x, (int)y);
        if (A.drag == M_LOOK) cam_look((float)dx, (float)dy);
        if (A.drag == M_PAN)  cam_pan((float)dx, (float)dy);
        return;
    }
    if (A.drag == M_LOOK) cam_look((float)dx, (float)dy);
    if (A.drag == M_PAN)  cam_pan((float)dx, (float)dy);
    if (A.drag == M_MARQUEE) { A.marqX1 = (float)x; A.marqY1 = (float)y; }
    if (A.drag == M_CLICK || A.drag == M_MOVE) edit_lmb_motion();
}

static void on_mouse(GLFWwindow *w, int button, int action, int mods) {
    (void)w;
    if (A.modal == 1) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            if (action == GLFW_PRESS) art_press((float)A.mx, (float)A.my);
            else art_release();
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            art_rmb(action == GLFW_PRESS, (float)A.mx, (float)A.my);
        }
        return;
    }
    if (A.modal == 2) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            if (action == GLFW_PRESS) sculpt_press((int)A.mx, (int)A.my);
            else sculpt_release();
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) {
                A.drag = M_LOOK; A.rmbX = A.mx; A.rmbY = A.my;
            } else {
                /* short click = split nearest edge; drag = camera look */
                float mdx = (float)(A.mx - A.rmbX), mdy = (float)(A.my - A.rmbY);
                if (mdx*mdx + mdy*mdy < 36.0f)
                    sculpt_rmb_click((int)A.mx, (int)A.my);
                A.drag = M_IDLE;
            }
        } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            A.drag = (action == GLFW_PRESS) ? M_PAN : M_IDLE;
        }
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            A.overPanel = panel_over(A.mx, A.my);
            if (panel_press(A.mx, A.my)) return;
            if (A.app == APP_EDIT && panel_script_click(A.mx, A.my)) return;
            if (A.app == APP_EDIT) edit_lmb_press(mods);
        } else {
            if (A.pickDrag == 3) panel_apply_paint();
            A.pickDrag = 0;
            if (A.app == APP_EDIT) edit_lmb_release(mods);
            else A.drag = M_IDLE;
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            if (A.app == APP_EDIT && panel_rmb(A.mx, A.my)) return;
            A.drag = M_LOOK; A.rmbX = A.mx; A.rmbY = A.my;
        }
        else {
            edit_rmb_up((float)(A.mx - A.rmbX), (float)(A.my - A.rmbY));
            A.drag = (A.app == APP_EDIT && A.armedAsset >= 0) ? M_ARMED : M_IDLE;
        }
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
    if (A.modal == 1) { art_scroll(yoff); return; }
    if (A.modal == 2) { sculpt_wheel(yoff); return; }
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
    if (A.modal == 1) { art_key(key); return; }
    if (A.modal == 2) { sculpt_key(key); return; }
    /* text input swallows keys first */
    if (A.textMode) {
        if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER) {
            if (A.textMode == 3) panel_rename_commit();
            A.textMode = 0;
        }
        else if (key == GLFW_KEY_BACKSPACE) {
            if (A.textMode == 1) { int n = (int)strlen(A.search); if (n) A.search[n-1] = 0; }
            else if (A.textMode == 2 && A.cmdLen) A.cmdBuf[--A.cmdLen] = 0;
            else if (A.textMode == 3) { int n = (int)strlen(A.renBuf); if (n) A.renBuf[n-1] = 0; }
        }
        return;
    }
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS) { prof_toggle(); settings_save(); return; }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) { toggle_fullscreen(); return; }
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
        toast_say(rk_reload_shaders() ? "shaders reloaded"
                                      : "shader reload: compile errors");
        return;
    }
    if (key == GLFW_KEY_F12 && action == GLFW_PRESS) {
        static int shotN;
        char p[64]; snprintf(p, sizeof p, "shot_%d.bmp", ++shotN);
        rk_screenshot(p); return;
    }
    if (key == GLFW_KEY_TAB && (action == GLFW_PRESS)) { app_toggle_mode(); return; }
    if (key == GLFW_KEY_Y && !(mods & GLFW_MOD_CONTROL)) {
        A.invertY = !A.invertY; settings_save(); return;
    }
    if (key == GLFW_KEY_MINUS || key == GLFW_KEY_EQUAL) {
        A.fov = clampf(A.fov + (key == GLFW_KEY_EQUAL ? -0.05f : 0.05f),
                       0.55f, 2.2f);
        settings_save(); return;
    }
    if (key == GLFW_KEY_G && action == GLFW_PRESS && !(mods & GLFW_MOD_CONTROL)) {
        A.guideOn = !A.guideOn; A.setDirty = 1;
        toast_say(A.guideOn ? "guide tips ON" : "guide tips OFF"); return;
    }
    if (key == GLFW_KEY_ESCAPE && panel_del_pending()) { panel_del_cancel(); return; }
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
#ifdef _WIN32
    /* render at real monitor pixels, not Windows-scaled logical size -
       without this the whole UI is blurry upscaled text */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    if (!glfwInit()) { fputs("glfwInit failed\n", stderr); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    A.win = glfwCreateWindow(1280, 720, "Varlera", NULL, NULL);
    if (!A.win) { fputs("window failed\n", stderr); return 1; }
    /* borderless fullscreen by default; settings.bw may restore the
       windowed rect instead (F11 toggles, rect + mode persist) */
    settings_load();
    if (A.winW > 0 && !A.fullscreen) {
        glfwSetWindowPos(A.win, A.winX, A.winY);
        glfwSetWindowSize(A.win, A.winW, A.winH);
    } else {
        if (A.winW <= 0) {          /* first run: remember this rect for F11 */
            glfwGetWindowPos(A.win, &A.winX, &A.winY);
            glfwGetWindowSize(A.win, &A.winW, &A.winH);
        }
        GLFWmonitor *m = glfwGetPrimaryMonitor();
        const GLFWvidmode *vm = glfwGetVideoMode(m);
        glfwSetWindowMonitor(A.win, m, 0, 0, vm->width, vm->height,
                             GLFW_DONT_CARE);
        A.fullscreen = 1;
    }
    glfwGetFramebufferSize(A.win, &A.fbw, &A.fbh);
    glfwSetInputMode(A.win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    glfwSetCursorPosCallback(A.win, on_cursor);
    glfwSetCharCallback(A.win, on_char);
    glfwSetMouseButtonCallback(A.win, on_mouse);
    glfwSetScrollCallback(A.win, on_scroll);
    glfwSetKeyCallback(A.win, on_key);
    glfwSetFramebufferSizeCallback(A.win, on_resize);

    if (!rk_init(A.win)) { fputs("rk_init failed\n", stderr); return 1; }

    cam_init();
    settings_apply();            /* speed/fov/invertY/profOn from file */
    panel_init();
    tex_init();                  /* atlas: built-ins + textures.bw */
    models_init();               /* reload saved custom models */
    world_load("world.bw");      /* auto-load last world if present */
    A.app = APP_EDIT;
    A.drag = M_IDLE;
    A.hoverBlock = A.hoverAxis = A.hoverAsset = -1;
    A.lastT = glfwGetTime();
    prof_reset();
    /* scripted capture: VARLERA_SHOT=out.bmp writes a screenshot at
       frame 45 then continues (handy for agent/CI verification) */
    const char *envShot = getenv("VARLERA_SHOT");
    const char *envReload = getenv("VARLERA_RELOAD");   /* test hook: F8 path */
    int frameNo = 0;

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

        prof_zone_begin(PZ_SIM);
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
        else if (A.app == APP_EDIT && !A.modal) { edit_update(); panel_drag_update(); }
        prof_zone_end(PZ_SIM);

        prof_zone_begin(PZ_WAIT);
        int ok = rk_begin_frame();
        prof_zone_end(PZ_WAIT);
        if (!ok) continue;

        v3 eye = A.camPos;
        v3 focus = v3_add(A.camPos, v3_scale(cam_basis().f, 30.0f));
        rk_set_camera(A.viewProj, eye.e, focus.e);

        prof_zone_begin(PZ_QUEUE);
        if (A.modal != 2) {                    /* sculpt = own mini-world */
            rk_draw_sky();
            rk_draw_grid();
            edit_draw_world();
        }
        if (A.modal == 1) art_draw();
        else if (A.modal == 2) sculpt_draw();
        else panel_draw();                               /* modal overlays editor */
        prof_draw();
        toast_frame(dt);
        toast_draw();
        rk_ui_flush();
        prof_zone_end(PZ_QUEUE);

        prof_zone_begin(PZ_END);
        rk_end_frame();
        prof_zone_end(PZ_END);
        ++frameNo;
        if (envShot && frameNo == 45) rk_screenshot(envShot);
        if (envReload && frameNo == 60) {
            toast_say(rk_reload_shaders() ? "shaders reloaded"
                                          : "shader reload: compile errors");
        }
        if (A.setDirty) { A.setDirty = 0; settings_save(); }

        prof_count(PC_BLOCKS, g_nb);
        RkStats rs; rk_query_stats(&rs);
        prof_count(PC_DRAWS, rs.draws);
        prof_count(PC_INST, rs.insts);
        prof_count(PC_UIVERTS, rs.uiVerts);
        prof_count(PC_MESHVERTS, rs.meshVerts);
        prof_count(PC_MODELS, rs.models);
        prof_frame();
    }

    settings_save();
    tex_save("textures.bw");
    rk_shutdown();
    glfwDestroyWindow(A.win);
    glfwTerminate();
    return 0;
}
