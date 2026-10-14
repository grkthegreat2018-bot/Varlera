/* panel_world.c - WORLD tab: name field, save/load/undo/clear buttons,
 * save dir + .bwx bundle pickers, sky/ground env presets (env.bw), world
 * settings rows (WSet trailer) with the wipe-warning confirm dialog.
 * Split out of panel.c; shared UI constants live in panel.h. */
#define _CRT_SECURE_NO_WARNINGS
#include "panel.h"
#include "app.h"
#include "world.h"
#include "tex.h"
#include "edit.h"
#include "font.h"
#include "toast.h"
#include "filepick.h"
#include "bundle.h"
#include "model.h"
#include <stdio.h>
#include <string.h>

/* ---- environment: sky + ground presets (world.bw-independent env.bw) ---- */
static const float ENV_SKY[][3] = {
    {0.56f,0.66f,0.82f}, {0.94f,0.60f,0.40f}, {0.36f,0.32f,0.56f},
    {0.05f,0.07f,0.16f}, {0.52f,0.82f,0.72f}, {0.02f,0.02f,0.04f},
};
static const char *ENV_SKY_N[] = {"DAY","SUNSET","DUSK","NIGHT","MINT","VOID"};
#define ENV_NSKY 6
static const float ENV_GROUND[][3] = {
    {0.58f,0.60f,0.64f}, {0.36f,0.60f,0.30f}, {0.80f,0.70f,0.48f},
    {0.88f,0.90f,0.95f}, {0.17f,0.18f,0.22f},
};
static const char *ENV_GROUND_N[] = {"CONCRETE","GRASS","SAND","SNOW","DARK"};
#define ENV_NGROUND 5
static const char *ENV_GSTYLE_N[] = {"GRID","CHECK","DOTS","PLAIN"};
#define ENV_NGSTYLE 4
static int s_sky = 0, s_ground = 0, s_gstyle = 0;

static void env_apply(void) {
    rk_set_env(ENV_SKY[s_sky], ENV_GROUND[s_ground], s_gstyle);
}
static void env_save(void) {
    FILE *f = fopen("env.bw", "wb");
    if (f) { int v[3] = {s_sky, s_ground, s_gstyle}; fwrite(v, 4, 3, f); fclose(f); }
}
void panel_env_load(void) {
    FILE *f = fopen("env.bw", "rb");
    if (!f) { env_apply(); return; }
    int v[3] = {0, 0, 0};
    if (fread(v, 4, 3, f) >= 2) {
        if (v[0] >= 0 && v[0] < ENV_NSKY) s_sky = v[0];
        if (v[1] >= 0 && v[1] < ENV_NGROUND) s_ground = v[1];
        if (v[2] >= 0 && v[2] < ENV_NGSTYLE) s_gstyle = v[2];
    }
    fclose(f);
    env_apply();
}

/* pending world-setting change that needs the wipe-warning confirm */
static int s_wConfirm = -1;
/* rows 8-11 = skybox object pickers (visible iff SKY OBJS on);
   rows 12-14 = EXPERIMENTAL render section (visible iff ADV RENDER on);
   row 15 = day/night cycle speed; row 16 = play-mode player */
#define WS_ROWS 17
static int ws_row_vis(int i) {
    const WSet *w = world_wset();
    if (i >= 12 && i <= 14) return w->advRender;
    if (i >= 8 && i <= 11)  return w->skyAssets;
    return 1;
}
static const char *WLBL[] = {"PHYS","CHUNK","CHUNK SZ","LOD","ADV RENDER",
                             "GRAVITY","DRAG","SKY OBJS","SUN","MOON",
                             "OBJ A","OBJ B","FOG START","FOG END","BLOOM",
                             "DAY CYCLE","PLAYER"};
static const char *WWARN[] = {
    "PHYSICS MODE CHANGES PLAY BEHAVIOR",
    "CHUNK LAYOUT MAY RESHAPE OR WIPE THIS WORLD",
    "CHUNK SIZE MAY RESHAPE OR WIPE THIS WORLD",
    "LOD ALTERS DRAW DISTANCE QUALITY",
    "EXPERIMENTAL RENDER FLAGS CAN CAUSE BUGS",
    "", "", "", "", "", "", "", "", "", "", "",
};
static const uint8_t GRAV_STEPS[] = {0, 2, 4, 8, 12, 16};   /* 0x..2x */
/* index names only — step tables/apply live with the record (world.c) */
static const char *FOGN_N[] = {"AUTO","30","90","150"};
static const char *FOGF_N[] = {"AUTO","120","260","400"};
static const char *BLOOM_N[] = {"AUTO","OFF","LOW","HIGH"};

static void wset_cycle(int i) {
    WSet *w = world_wset();
    switch (i) {
    case 0: w->phys = (uint8_t)((w->phys + 1) % 2); break;
    case 1: w->chunkShape = (uint8_t)((w->chunkShape + 1) % 3); break;
    case 2: w->chunkSize = w->chunkSize == 8 ? 16 :
                          w->chunkSize == 16 ? 32 : 8; break;
    case 3: w->lod ^= 1; break;
    case 4: w->advRender ^= 1; break;
    case 5: { int s = 0;
              while (s < 6 && GRAV_STEPS[s] != w->gravMul) s++;   /* find */
              w->gravMul = GRAV_STEPS[(s + 1) % 6];              /* advance */
              break; }
    case 6: w->dragOff ^= 1; break;
    case 7: w->skyAssets ^= 1; break;
    case 8: case 9: case 10: case 11: {     /* sky object model pickers */
        int k = i - 8;
        w->skyObj[k] = (uint8_t)((w->skyObj[k] + 1) % (g_nmodels + 1));
        break; }
    case 12: w->fogNear = (uint8_t)((w->fogNear + 1) & 3); break;
    case 13: w->fogFar  = (uint8_t)((w->fogFar + 1) & 3); break;
    case 14: w->bloom   = (uint8_t)((w->bloom + 1) & 3); break;
    case 15: w->dayCycle = (uint8_t)((w->dayCycle + 1) & 3); break;
    case 16: w->playerMode ^= 1; break;
    }
    world_wset_dirty();
}
static const char *wset_val(int i, const WSet *w, char *b) {
    static const char *PH[] = {"SETTLE","RIGID"};
    static const char *CH[] = {"NONE","COLUMN","CUBIC"};
    static const char *ON[] = {"OFF","ON"};
    switch (i) {
    case 0: return PH[w->phys & 1];
    case 1: return CH[w->chunkShape % 3];
    case 2: snprintf(b, 8, "%d", w->chunkSize ? w->chunkSize : 16); return b;
    case 3: return ON[w->lod & 1];
    case 4: return ON[w->advRender & 1];
    case 5: snprintf(b, 8, "%.2fx", (w->gravMul ? w->gravMul : 8) / 8.0f);
            return b;
    case 6: return ON[(w->dragOff ^ 1) & 1];   /* ON = dragging allowed */
    case 7: return ON[w->skyAssets & 1];
    case 8: case 9: case 10: case 11: {
        int v = w->skyObj[i - 8];
        if (v <= 0) return "NONE";
        snprintf(b, 8, "%.7s", g_models[v - 1].name); return b; }
    case 12: return FOGN_N[w->fogNear & 3];
    case 13: return FOGF_N[w->fogFar & 3];
    case 14: return BLOOM_N[w->bloom & 3];
    case 15: { static const char *DL[] = {"OFF","SLOW","MED","FAST"};
              return DL[w->dayCycle & 3]; }
    case 16: { static const char *PM[] = {"FLY","WALK"};
              return PM[w->playerMode & 1]; }
    }
    return "";
}

/* ---- press handling (called only when A.panelTab == 3) ---- */
int panel_world_tab_press(float fx, float fy, float px) {
    if (in_rect(fx, fy, px + 8, 58, PW - 16, 20)) { A.textMode = 4; return 1; }
    for (int i = 0; i < 9; i++) {
        float bx = px + 8 + (i%3)*68.0f, byy = 80.0f + (i/3)*30.0f;
        if (in_rect(fx, fy, bx, byy, 62, 24)) {
            switch (i) {
            case 0: edit_quicksave(); break;
            case 1: if (world_load(worlds_file(A.worldName, "bw"), A.worldName) ||
                        world_load("world.bw", A.worldName)) sel_clear(); break;
            case 2: if (world_undo()) sel_clear(); break;
            case 3: if (world_redo()) sel_clear(); break;
            case 4: world_push_undo(); g_nb = 0; sel_clear(); break;
            case 5: { char p[512];
                      if (pick_save_path(p, sizeof p, "Save world as",
                                         "Varlera World\0*.bw\0All Files\0*.*\0", "bw")) {
                          world_save(p, A.worldName); tex_save("textures.bw");
                      } break; }
            case 6: { char d[384];
                      if (pick_dir(d, sizeof d)) {
                          worlds_set_dir(d); worlds_ensure(); A.setDirty = 1;
                          toast_say("SAVE DIR SET"); } break; }
            case 7: { char p[512];
                      if (pick_save_path(p, sizeof p, "Export project bundle",
                                         "Varlera Bundle\0*.bwx\0All Files\0*.*\0", "bwx") &&
                          bundle_save(p, A.worldName))
                          toast_say("EXPORTED .BWX"); } break;
            case 8: { char p[512];
                      if (pick_open_path(p, sizeof p, "Import project bundle",
                                         "Varlera Bundle\0*.bwx\0All Files\0*.*\0") &&
                          bundle_load(p, A.worldName))
                          sel_clear(); } break;
            }
            return 1;
        }
    }
    /* environment pickers: SKY / GROUND / FLOOR cycle */
    for (int i = 0; i < 3; i++) {
        float ey = 226.0f + i * 34.0f;
        if (in_rect(fx, fy, px + 8, ey, PW - 16, 26)) {
            if (i == 0)      s_sky = (s_sky + 1) % ENV_NSKY;
            else if (i == 1) s_ground = (s_ground + 1) % ENV_NGROUND;
            else             s_gstyle = (s_gstyle + 1) % ENV_NGSTYLE;
            env_apply(); env_save();
            return 1;
        }
    }
    /* world settings — rows 1/2/4 are dangerous: confirm first */
    for (int i = 0, r = 0; i < WS_ROWS; i++) {
        if (!ws_row_vis(i)) continue;
        float wy = 346.0f + (float)r++ * 28.0f;
        if (in_rect(fx, fy, px + 8, wy, PW - 16, 24)) {
            if (i == 1 || i == 2 || i == 4) s_wConfirm = i;
            else wset_cycle(i);
            return 1;
        }
    }
    return 1;
}

/* wipe-warning dialog eats every click while open; APPLY sits left */
int panel_world_confirm_click(float x, float y) {
    if (s_wConfirm < 0) return 0;
    float cx = ((float)A.fbw - 320.0f) * 0.5f;
    float cy2 = (float)A.fbh * 0.5f - 55.0f;
    if (in_rect(x, y, cx + 30, cy2 + 72, 110, 26))
        wset_cycle(s_wConfirm);         /* APPLY */
    s_wConfirm = -1;                    /* any click dismisses */
    return 1;
}

void panel_world_draw(float px) {
    /* world name field -> worlds/<name>.bw (textMode 4) */
    int foc = A.textMode == 4;
    rk_ui_rect(px + 8, 58, PW - 16, 20,
               foc ? 0.24f : 0.15f, foc ? 0.26f : 0.16f, foc ? 0.34f : 0.21f, 1);
    rk_ui_rect(px + 8, 58, PW - 16, 1.5f, 0.4f,0.6f,0.9f, foc ? 0.9f : 0.4f);
    char nb[56];
    snprintf(nb, sizeof nb, "NAME: %s%s", A.worldName, foc ? "_" : "");
    ui_text(px + 14, 64, nb, 1,
            foc ? 1.0f : 0.75f, foc ? 1.0f : 0.78f, foc ? 1.0f : 0.85f, 1);
    const char *names[] = {"SAVE","LOAD","UNDO","REDO","CLEAR","SAVEAS",
                           "DIR","EXPORT","IMPORT"};
    for (int i = 0; i < 9; i++) {
        float bx = px + 8 + (i%3)*68;
        float by = 80.0f + (i/3)*30.0f;
        rk_ui_rect(bx, by, 62, 24, 0.22f,0.30f,0.45f, 1);
        ui_text(bx + (62 - ui_text_w(names[i],1))/2, by + 8, names[i], 1, 1,1,1, 1);
    }
    char buf[48];
    snprintf(buf, sizeof buf, "BLOCKS %d / %d", g_nb, MAX_BLOCKS);
    ui_text(px+12, 172, buf, 1, 0.75f,0.78f,0.85f, 1);
    /* save dir tail (fits ~24 chars) */
    {
        const char *d = worlds_dir(); int dl = (int)strlen(d);
        const char *tail = dl > 22 ? d + dl - 22 : d;
        snprintf(buf, sizeof buf, "DIR ...%s", tail);
        ui_text(px+12, 190, buf, 1, 0.55f,0.6f,0.68f, 1);
    }

    /* environment: sky + ground presets + floor pattern */
    ui_text(px + 12, 212, "ENVIRONMENT", 1, 0.7f,0.72f,0.8f, 1);
    const char *envN[3] = {ENV_SKY_N[s_sky], ENV_GROUND_N[s_ground],
                           ENV_GSTYLE_N[s_gstyle]};
    const char *envL[3] = {"SKY:", "GROUND:", "FLOOR:"};
    for (int i = 0; i < 3; i++) {
        float ey = 226.0f + i * 34.0f;
        int hov = in_rect((float)A.mx,(float)A.my, px + 8, ey, PW - 16, 26);
        rk_ui_rect(px + 8, ey, PW - 16, 26,
                   0.22f + hov*0.08f, 0.28f + hov*0.08f, 0.38f + hov*0.08f, 1);
        char eb[32]; snprintf(eb, sizeof eb, "%s %s", envL[i], envN[i]);
        ui_text(px + 16, ey + 9, eb, 1, 1,1,1, 1);
        /* color swatch on the right edge (no swatch for the pattern row) */
        if (i < 2) {
            const float *ec = i == 0 ? ENV_SKY[s_sky] : ENV_GROUND[s_ground];
            rk_ui_rect(px + PW - 32, ey + 5, 18, 16, ec[0],ec[1],ec[2], 1);
        }
    }

    /* world settings — ! marks rows that warn before changing */
    ui_text(px + 12, 330, "WORLD SETTINGS", 1, 0.7f,0.72f,0.8f, 1);
    {
        const WSet *w = world_wset();
        for (int i = 0, r = 0; i < WS_ROWS; i++) {
            if (!ws_row_vis(i)) continue;
            float wy = 346.0f + (float)r++ * 28.0f;
            int hov = in_rect((float)A.mx,(float)A.my, px+8, wy, PW-16, 24);
            int danger = (i == 1 || i == 2 || i == 4);
            rk_ui_rect(px+8, wy, PW-16, 24,
                       0.20f + hov*0.07f, 0.24f + hov*0.07f, 0.32f + hov*0.07f, 1);
            char wb[48], vb[8];
            snprintf(wb, sizeof wb, "%s%s: %s",
                     danger ? "! " : "", WLBL[i], wset_val(i, w, vb));
            ui_text(px + 14, wy + 8, wb, 1,
                    danger ? 1.0f : 0.85f, danger ? 0.75f : 0.85f,
                    danger ? 0.55f : 0.9f, 1);
        }
    }
    rk_ui_flush();
}

void panel_world_confirm_draw(void) {
    if (s_wConfirm < 0) return;
    float cx = ((float)A.fbw - 320.0f) * 0.5f;
    float cy2 = (float)A.fbh * 0.5f - 55.0f;
    rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0,0,0, 0.45f);
    rk_ui_rect(cx, cy2, 320, 110, 0.13f,0.14f,0.17f, 1);
    rk_ui_rect(cx, cy2, 320, 2, 1.0f,0.6f,0.2f, 1);
    char db[48];
    snprintf(db, sizeof db, "CHANGE %s?", WLBL[s_wConfirm]);
    ui_text(cx + (320 - ui_text_w(db,2)) * 0.5f, cy2 + 14, db, 2, 1,1,1, 1);
    ui_text(cx + (320 - ui_text_w(WWARN[s_wConfirm],1)) * 0.5f, cy2 + 44,
            WWARN[s_wConfirm], 1, 1.0f,0.7f,0.4f, 1);
    int hovY = in_rect((float)A.mx,(float)A.my, cx + 30, cy2 + 72, 110, 26);
    rk_ui_rect(cx + 30, cy2 + 72, 110, 26,
               0.65f + hovY*0.15f, 0.22f, 0.20f, 1);
    ui_text(cx + 30 + (110 - ui_text_w("APPLY",1)) * 0.5f, cy2 + 80,
            "APPLY", 1, 1,1,1, 1);
    int hovN = in_rect((float)A.mx,(float)A.my, cx + 180, cy2 + 72, 110, 26);
    rk_ui_rect(cx + 180, cy2 + 72, 110, 26,
               0.22f + hovN*0.08f, 0.26f + hovN*0.08f, 0.34f + hovN*0.08f, 1);
    ui_text(cx + 180 + (110 - ui_text_w("CANCEL",1)) * 0.5f, cy2 + 80,
            "CANCEL", 1, 1,1,1, 1);
    rk_ui_flush();
}
