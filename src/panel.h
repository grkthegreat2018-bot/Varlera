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
const float *panel_color(int i);    /* palette rgb */

void panel_init(void);
int  panel_over(double x, double y);        /* cursor over any UI chrome */
int  panel_press(double x, double y);       /* LMB press; 1 if consumed */
void panel_drag_update(void);               /* per-frame while pickDrag active */
void panel_scroll(double dy);
void panel_draw(void);                      /* queues UI + icons (+ tooltip) */
int  panel_script_click(double x, double y); /* block context menu; 1 if consumed */
int  panel_rmb(double x, double y);          /* RMB: delete custom model; 1 if consumed */

#endif
