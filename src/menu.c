/* menu.c - main menu overlay: world browser (worlds_dir scan), resume /
   new / import / quit. Draws over the still-rendered world. */
#include "menu.h"
#include "app.h"
#include "world.h"
#include "bundle.h"
#include "filepick.h"
#include "edit.h"
#include "rk.h"
#include "panel.h"
#include "font.h"
#include "toast.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MENU_MAX 64

static char  s_name[MENU_MAX][32];
static int   s_bwx[MENU_MAX];
static long  s_mtime[MENU_MAX];
static int   s_n, s_sel = -1;
static float s_scroll;

static void menu_scan(void) {
    s_n = 0; s_sel = -1; s_scroll = 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", worlds_dir());
#ifdef _WIN32
    char pat[560]; snprintf(pat, sizeof pat, "%s\\*.*", dir);
    struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        const char *f = fd.name;
        int isBw  = strstr(f, ".bw")  && !strstr(f, ".auto.bw") && strlen(f) > 3;
        int isBwx = strstr(f, ".bwx") != NULL;
        /* *.bw check: ends in .bw (finddata is case-sensitive enough) */
        if (isBw) { size_t L = strlen(f); isBw = L >= 3 && !strcmp(f + L - 3, ".bw"); }
        if ((!isBw && !isBwx) || s_n >= MENU_MAX) continue;
        snprintf(s_name[s_n], 32, "%.*s", 28, f);
        char *dot = strrchr(s_name[s_n], '.'); if (dot) *dot = 0;
        s_bwx[s_n] = isBwx;
        s_mtime[s_n] = (long)fd.time_write;
        s_n++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_n < MENU_MAX) {
        const char *f = de->d_name; size_t L = strlen(f);
        int isBw  = L >= 3 && !strcmp(f + L - 3, ".bw") && !strstr(f, ".auto.bw");
        int isBwx = L >= 4 && !strcmp(f + L - 4, ".bwx");
        if (!isBw && !isBwx) continue;
        snprintf(s_name[s_n], 32, "%.*s", 28, f);
        char *dot = strrchr(s_name[s_n], '.'); if (dot) *dot = 0;
        s_bwx[s_n] = isBwx;
        char fp[560]; snprintf(fp, sizeof fp, "%s/%s", dir, f);
        struct stat st; s_mtime[s_n] = stat(fp, &st) ? 0 : (long)st.st_mtime;
        s_n++;
    }
    closedir(d);
#endif
    /* newest first (insertion sort, <=64 entries) */
    for (int i = 1; i < s_n; i++) {
        char nm[32]; memcpy(nm, s_name[i], 32);
        int bx = s_bwx[i]; long mt = s_mtime[i]; int j = i;
        while (j > 0 && s_mtime[j-1] < mt) {
            memcpy(s_name[j], s_name[j-1], 32);
            s_bwx[j] = s_bwx[j-1]; s_mtime[j] = s_mtime[j-1]; j--;
        }
        memcpy(s_name[j], nm, 32); s_bwx[j] = bx; s_mtime[j] = mt;
    }
}

void menu_enter(void) {
    worlds_ensure();
    menu_scan();
    A.app = APP_MENU;
    A.drag = M_IDLE;
    A.textMode = 0;
}

static void open_entry(int i) {
    if (i < 0 || i >= s_n) return;
    int ok = s_bwx[i] ? bundle_load(worlds_file(s_name[i], "bwx"), A.worldName)
                      : world_load(worlds_file(s_name[i], "bw"), A.worldName);
    char msg[48];
    if (ok) { sel_clear(); A.app = APP_EDIT;
              snprintf(msg, sizeof msg, "loaded %s", s_name[i]); }
    else snprintf(msg, sizeof msg, "load failed: %s", s_name[i]);
    toast_say(msg);
}

/* layout constants */
#define MW  460.0f            /* panel width  */
#define ROW 26.0f
#define VISROWS 12

static float menu_x(void) { return ((float)A.fbw - MW) * 0.5f; }
static float list_y(void) { return (float)A.fbh * 0.5f - 140.0f; }

void menu_press(double x, double y) {
    float fx = (float)x, fy = (float)y, mx = menu_x(), ly = list_y();
    /* world rows */
    for (int v = 0; v < VISROWS && v < s_n; v++) {
        int i = v + (int)s_scroll;
        if (i >= s_n) break;
        if (in_rect(fx, fy, mx + 10, ly + 44 + v * ROW, MW - 20, ROW - 3)) {
            if (s_sel == i) open_entry(i);          /* 2nd click opens */
            else s_sel = i;
            return;
        }
    }
    /* buttons row */
    float by = ly + 44 + VISROWS * ROW + 12;
    if (in_rect(fx, fy, mx + 10, by, 100, 30)) { A.app = APP_EDIT; return; }       /* RESUME */
    if (in_rect(fx, fy, mx + 120, by, 100, 30)) {                                /* NEW */
        world_push_undo(); g_nb = 0; sel_clear();
        snprintf(A.worldName, sizeof A.worldName, "new world");
        A.app = APP_EDIT; toast_say("new world"); return;
    }
    if (in_rect(fx, fy, mx + 230, by, 110, 30)) {                                /* IMPORT */
        char p[512];
        if (pick_open_path(p, sizeof p, "Import bundle", "Varlera bundle\0*.bwx\0")
            && bundle_load(p, A.worldName)) { sel_clear(); A.app = APP_EDIT; }
        return;
    }
    if (in_rect(fx, fy, mx + MW - 100, by, 90, 30))                              /* QUIT */
        glfwSetWindowShouldClose(A.win, 1);
}

void menu_key(int key) {
    if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER && s_sel < 0)
        A.app = APP_EDIT;
    else if (key == GLFW_KEY_ENTER && s_sel >= 0) open_entry(s_sel);
    else if (key == GLFW_KEY_UP   && s_sel > 0) s_sel--;
    else if (key == GLFW_KEY_DOWN && s_sel + 1 < s_n) s_sel++;
    else if (key == GLFW_KEY_TAB)  A.app = APP_EDIT;
    if (s_sel >= 0) {
        if (s_sel < (int)s_scroll) s_scroll = (float)s_sel;
        if (s_sel >= (int)s_scroll + VISROWS) s_scroll = (float)(s_sel - VISROWS + 1);
    }
}

void menu_wheel(double y) {
    s_scroll -= (float)y;
    float maxs = (float)(s_n - VISROWS); if (maxs < 0) maxs = 0;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > maxs) s_scroll = maxs;
}

static void mbtn(float x, float y, float w, const char *t, int acc) {
    int hov = in_rect((float)A.mx, (float)A.my, x, y, w, 30);
    rk_ui_rect(x, y, w, 30, acc ? 0.25f : 0.20f + hov * 0.08f,
               acc ? 0.55f : 0.24f + hov * 0.08f,
               acc ? 0.35f : 0.30f + hov * 0.08f, 1);
    ui_text(x + (w - (float)ui_text_w(t, 2)) * 0.5f, y + 8, t, 2, 1,1,1, 1);
}

void menu_draw(void) {
    float mx = menu_x(), ly = list_y();
    float mh = 44 + VISROWS * ROW + 12 + 44;
    rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0.03f, 0.04f, 0.06f, 0.72f);
    rk_ui_rect(mx, ly - 70, MW, mh + 70, 0.12f, 0.13f, 0.17f, 0.97f);
    rk_ui_rect(mx, ly - 70, MW, 2, 1,1,1, 0.4f);
    ui_text(mx + 16, ly - 58, "VARLERA", 3, 0.9f, 0.92f, 1.0f, 1);
    ui_text(mx + 16, ly - 26, "worlds", 1, 0.7f, 0.72f, 0.8f, 0.9f);
    ui_text(mx + MW - 90, ly - 26, "modified", 1, 0.7f, 0.72f, 0.8f, 0.9f);
    if (!s_n)
        ui_text(mx + 16, ly + 50, "no saved worlds yet - NEW to start",
                1, 0.8f, 0.8f, 0.85f, 0.9f);
    for (int v = 0; v < VISROWS; v++) {
        int i = v + (int)s_scroll; if (i >= s_n) break;
        float ry = ly + 44 + v * ROW;
        int sel = (s_sel == i);
        int hov = in_rect((float)A.mx, (float)A.my, mx + 10, ry, MW - 20, ROW - 3);
        rk_ui_rect(mx + 10, ry, MW - 20, ROW - 3,
                   sel ? 0.28f : 0.16f + hov * 0.06f,
                   sel ? 0.40f : 0.17f + hov * 0.06f,
                   sel ? 0.55f : 0.21f + hov * 0.07f, 1);
        ui_text(mx + 18, ry + 7, s_name[i], 1, 1,1,1, 1);
        if (s_bwx[i]) ui_text(mx + 18 + ui_text_w(s_name[i], 1) + 6, ry + 7,
                              "[bwx]", 1, 0.6f, 0.8f, 1.0f, 1);
        if (s_mtime[i]) {
            char db[20]; time_t tt = (time_t)s_mtime[i];
#ifdef _WIN32
            struct tm tmv; localtime_s(&tmv, &tt);
            strftime(db, sizeof db, "%Y-%m-%d %H:%M", &tmv);
#else
            strftime(db, sizeof db, "%Y-%m-%d %H:%M", localtime(&tt));
#endif
            ui_text(mx + MW - 96, ry + 7, db, 1, 0.65f, 0.67f, 0.75f, 1);
        }
    }
    float by = ly + 44 + VISROWS * ROW + 12;
    mbtn(mx + 10,        by, 100, "RESUME", 1);
    mbtn(mx + 120,       by, 100, "NEW",    0);
    mbtn(mx + 230,       by, 110, "IMPORT", 0);
    mbtn(mx + MW - 100,  by, 90,  "QUIT",   0);
}
