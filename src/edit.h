/* edit.h - block editing: pick/place/move/resize/marquee/handles/draw. */
#ifndef EDIT_H
#define EDIT_H

void edit_lmb_press(int mods);      /* called on LMB press in world space */
void edit_lmb_release(int mods);
void edit_lmb_motion(void);         /* click->drag threshold promotion */
void edit_rmb_up(float dx, float dy); /* RMB release (click = eyedropper) */
void edit_key(int key, int mods);   /* R, Del, Ctrl+Z/Y/D/A, F5/F9, F, Esc */
void edit_update(void);             /* per-frame ghost/move/resize/hover */
void edit_draw_world(void);         /* push blocks, ghosts, lines, handles */

void sel_clear(void);
int  sel_single(void);
int  sel_first(void);
void delete_selected(void);
void fuse_selection(void);        /* merge selection into a baked model */
void unfuse_block(int i);         /* explode a fused model into children */

#endif
