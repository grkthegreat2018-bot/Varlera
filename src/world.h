/* world.h - block storage, undo, save/load, play-mode physics. */
#ifndef WORLD_H
#define WORLD_H

#include "math3d.h"
#include <stdint.h>

#define MAX_BLOCKS 4096

typedef struct {
    v3 pos, size;               /* size = logical (pre-rotation) */
    float col[3];
    float alpha;                /* <1 = translucent block */
    uint32_t id;                /* stable instance id (script targeting) */
    char name[12];              /* custom name (script targeting) */
    uint8_t shape, rot;         /* rot: 0..3 = k*90deg about Y */
    uint8_t tex, texm;          /* tex: atlas id; texm: TEXM_* mode|type bits */
    uint8_t flags;
    uint8_t trig, act, actParam, tgt;  /* BW-style script row */
    uint8_t pad[3];
} Block;                        /* 64 bytes */

/* Block.flags bits */
#define BF_LOCKED   1           /* can't be selected/moved/edited */
#define BF_NOCOLLIDE 2          /* physics: no support from/blocks */
#define BF_NOPHYS   4           /* exempt from play-mode gravity */
#define BF_SPIN     8           /* legacy: always spin (migrated to trig/act) */
#define BF_GLASS    16          /* translucent render */

/* Block.tgt values (script target scope) */
#define TGT_SELF   0            /* action applies to this block */
#define TGT_ASSET  1            /* all blocks with same asset/shape */
#define TGT_NAME   2            /* all blocks sharing custom name */
#define TGT_ALL    3            /* every block in the world */

/* Block.trig values */
#define TRIG_ALWAYS 0
#define TRIG_SPACE  1
#define TRIG_W      2
#define TRIG_A      3
#define TRIG_S      4
#define TRIG_D      5
#define TRIG_E      6
#define TRIG_Q      7
#define TRIG_CLICK  8           /* LMB while playing */
#define N_TRIGS     9

/* Block.act values */
#define ACT_NONE    0
#define ACT_SPIN    1
#define ACT_GLOW    2
#define ACT_BOOST   3           /* thruster: rises while trigger held */
#define ACT_COLOR   4           /* actParam = palette-ish color index (cycle) */
#define ACT_TEX     5           /* actParam = texture id */
#define ACT_DEL     6           /* delete block while triggered */
#define N_ACTS      7

extern Block g_blocks[MAX_BLOCKS];
extern int g_nb;
extern uint32_t g_nextid;       /* instance-id allocator */

/* effective AABB half-extents considering rot (x/z swap on odd rot) */
static inline v3 block_eff_size(const Block *b) {
    return (b->rot & 1) ? v3_make(b->size.z, b->size.y, b->size.x) : b->size;
}

int  world_add(v3 pos, v3 size, int shape, int rot, const float col[3]);
int  world_addb(const Block *src);  /* raw insert, copies every field */
void world_delete(int i);       /* swap-remove; caller fixes sel[] */

void world_push_undo(void);
int  world_undo(void);
int  world_redo(void);

int  world_save(const char *path);
int  world_load(const char *path);
void world_block_migrate(Block *b, const uint8_t *rec, int ver); /* old rec sizes */

/* play-mode gravity settle + script actions; call each frame while playing */
void world_physics(float dt, uint16_t trigHeld);
void world_reset_physics(void); /* restores pre-play transforms */

void world_snapshot_play(void); /* capture state before physics mutates it */
void world_restore_play(void);

#endif
