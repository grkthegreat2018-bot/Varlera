/* panel.h - right-side asset library UI + palette + mode button. */
#ifndef PANEL_H
#define PANEL_H

#include "rk.h"

typedef struct {
    const char *name;
    const char *tip;
    int mesh;            /* RkMesh */
    v3 size;
} Asset;

const Asset *panel_asset(int i);    /* NULL if out of range */
int panel_asset_cat(int i);         /* 0 basic,1 ramps,2 special,3 model, -1 none */
const float *panel_color(int i);    /* palette rgb */

#define PW 236.0f                            /* expanded panel width */

static inline int in_rect(float x, float y,
                          float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx+rw && y >= ry && y < ry+rh;
}

float panel_x(void);                        /* left edge of the panel */

void panel_init(void);
void panel_env_load(void);                  /* env.bw preset load (panel_init) */
int  panel_over(double x, double y);        /* cursor over any UI chrome */
int  panel_press(double x, double y);       /* LMB press; 1 if consumed */
void panel_drag_update(void);               /* per-frame while pickDrag active */
void panel_scroll(double dy);
void panel_draw(void);                      /* queues UI + icons (+ tooltip) */
int  panel_script_click(double x, double y); /* block context menu; 1 if consumed */
void panel_rename_commit(void);              /* textMode 3 exit -> apply name */
int  panel_rmb(double x, double y);          /* RMB: delete custom model; 1 if consumed */
int  panel_del_pending(void);                 /* delete-confirm dialog open */
void panel_del_cancel(void);                  /* ESC dismiss */
void panel_apply_paint(void);                 /* commit dragged alpha to sel */

/* WORLD tab lives in panel_world.c — panel delegates these four hooks. */
int  panel_world_tab_press(float fx, float fy, float px); /* ->1 (swallows) */
int  panel_world_confirm_click(float x, float y);         /* ->1 if dialog ate it */
void panel_world_draw(float px);
void panel_world_confirm_draw(void);

#endif
