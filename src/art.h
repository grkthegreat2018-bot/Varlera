/* art.h - modal pixel-art tool: paints atlas texture pixels.
 * Art resolution: 4 atlas px per art pixel -> 32 cells per tile edge.
 * A 1x1 texture is a 32x32 canvas; a 3x3 sheet is 96x96 with tile guides. */
#ifndef ART_H
#define ART_H

void art_open(int texId);       /* enter modal on a texture (id >= 1) */
void art_close(int save);       /* leave; save=1 commits atlas + file */
int  art_press(float x, float y);   /* returns 1 always (modal swallows) */
void art_release(void);
void art_motion(void);          /* drag-paint while LMB held */
void art_rmb(int down, float x, float y);  /* RMB canvas pan */
void art_scroll(double dy);
int  art_key(int key);          /* returns 1 if consumed */
void art_draw(void);

#endif
