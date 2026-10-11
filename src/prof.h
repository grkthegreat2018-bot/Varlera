/* prof.h - frame cost logging. CPU zone timers + counters, GPU pass
 * timestamps (via rk), rolling stats, periodic stderr report, and a
 * toggleable on-screen overlay (F3).
 *
 * Usage in the frame loop:
 *   prof_zone_begin(PZ_x); ...work...; prof_zone_end(PZ_x);
 *   prof_count(PC_*, n);
 *   prof_frame();            // once per rendered frame */
#ifndef PROF_H
#define PROF_H

enum { PZ_WAIT,    /* rk_begin_frame: fence wait + acquire (GPU-bound tell) */
       PZ_SIM,     /* camera, edit update, play-mode physics */
       PZ_QUEUE,   /* CPU-side draw submission (edit_draw_world/ui flush) */
       PZ_END,     /* rk_end_frame: record + submit + present */
       PZ_N };

enum { PC_BLOCKS, PC_DRAWS, PC_INST, PC_UIVERTS, PC_MESHVERTS, PC_MODELS,
       PC_N };

void prof_reset(void);
void prof_zone_begin(int z);
void prof_zone_end(int z);
void prof_count(int c, int v);
void prof_frame(void);      /* call once per rendered frame */

void prof_toggle(void);     /* F3 */
int  prof_shown(void);
void prof_draw(void);       /* overlay; call before rk_ui_flush */

#endif
