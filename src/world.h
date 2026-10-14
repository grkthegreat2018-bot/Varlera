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
    uint8_t traction, bounce, weight;  /* physics stats, 0..15 (defaults
                                          BS_TRACT/BS_BOUNCE/BS_WEIGHT) */
} Block;
#define BS_TRACT  12            /* grippy by default */
#define BS_BOUNCE  0
#define BS_WEIGHT 8

/* Block.flags bits */
#define BF_LOCKED   1           /* can't be selected/moved/edited */
#define BF_NOCOLLIDE 2          /* physics: no support from/blocks */
#define BF_NOPHYS   4           /* exempt from play-mode gravity */
#define BF_SPIN     8           /* legacy: always spin (migrated to trig/act) */
#define BF_GLASS    16          /* translucent render */
#define BF_MERGED   32          /* play mode: drawn via greedy cluster mesh */
#define BF_SPAWN    64          /* player spawn point (walk mode) */
#define BF_GOAL     128         /* player touch -> goal event */

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
#define ACT_SAY     7           /* toast g_msgs[actParam] on trigger edge */
#define ACT_TITLE   8           /* same, styled as a title card */
#define N_ACTS      9

/* world message table (ACT_SAY/ACT_TITLE text slots) */
#define MSG_MAX 16
#define MSG_LEN 32
extern char g_msgs[MSG_MAX][MSG_LEN];

/* per-block dialog strings (BWST trailer): SAY/TITLE blocks may carry
 * their own text keyed by stable Block.id; empty -> falls back to
 * g_msgs[actParam]. */
#define BMSG_MAX 64
#define BMSG_LEN 48
typedef struct { uint32_t id; char text[BMSG_LEN]; } BMsg;
const char *world_block_msg(uint32_t id);      /* NULL if no override */
char       *world_block_msg_edit(uint32_t id); /* mutable slot, may NULL */

extern Block g_blocks[MAX_BLOCKS];
extern int g_nb;
extern uint32_t g_nextid;       /* instance-id allocator */
extern const float g_palette[16][3];  /* shared color vocabulary (UI + ACT_COLOR) */

/* effective AABB half-extents considering rot (x/z swap on odd rot) */
static inline v3 block_eff_size(const Block *b) {
    return (b->rot & 1) ? v3_make(b->size.z, b->size.y, b->size.x) : b->size;
}

/* XZ overlap of two center+extent AABBs; eps shrinks the test box */
static inline int aabb_overlap_xz(v3 pa, v3 ea, v3 pb, v3 eb, float eps) {
    return fabsf(pa.x - pb.x) < (ea.x + eb.x) * 0.5f - eps &&
           fabsf(pa.z - pb.z) < (ea.z + eb.z) * 0.5f - eps;
}

int  world_add(v3 pos, v3 size, int shape, int rot, const float col[3]);
int  world_addb(const Block *src);  /* raw insert, copies every field */
void world_delete(int i);       /* swap-remove; caller fixes sel[] */

void world_push_undo(void);
int  world_undo(void);
int  world_redo(void);

int  world_save(const char *path, const char *name);
int  world_load(const char *path, char *nameOut /*32, may be NULL*/);
/* async save: snapshot g_blocks (~280KB memcpy) then serialize+LZSS+write
 * on a worker thread. -1 if a save is already in flight. */
int  world_save_async(const char *path, const char *name);
int  world_save_poll(void);   /* 1 running, 0 ok, -1 failed, 2 none */
uint64_t world_rev(void);        /* mutation counter -> autosave dirty check */
const char *worlds_file(const char *name, const char *ext); /* <dir>/<san>.<ext> */
const char *worlds_dir(void);           /* current save dir (default worlds/) */
void worlds_set_dir(const char *d);     /* user-chosen save folder */
void worlds_ensure(void);               /* mkdir save dir if missing */
void world_block_migrate(Block *b, const uint8_t *rec, int ver); /* old rec sizes */
/* nearest block hit by a ray (scaled unit-AABB slabs; BF_LOCKED skipped).
 * axis = hit face 0-2 or -1 for inside-origin hits. */
int  world_pick_block(v3 ro, v3 rd, int *idx, float *tOut,
                      int *axisOut, float *signOut);
int  ray_unit_box(v3 ro, v3 rd, float *tOut, int *axisOut,
                  float *signOut);   /* shared slab test */

/* per-world settings — v4 trailer after block records (older readers stop
 * at EOF and get defaults). Fields marked dangerous in the UI reshape or
 * invalidate world data; the panel warns before changing them. */
typedef struct {
    uint32_t magic;          /* 'BWS4' */
    uint8_t  phys;           /* WS_PHYS_* : 0 arcade settle, 1 rigid bodies */
    uint8_t  chunkShape;     /* 0 none, 1 column (MC-style), 2 cubic */
    uint8_t  chunkSize;      /* cells per axis: 0/8/16/32 */
    uint8_t  lod;            /* 0 off, 1 on */
    uint8_t  advRender;      /* EXPERIMENTAL render-flag gate */
    uint8_t  gravMul;        /* gravity scale in eighths (8 = 1.0x) */
    uint8_t  skyAssets;      /* 0 preset sky, 1 user skybox objects */
    uint8_t  dragOff;        /* 1 disables play-mode LMB block dragging */
    uint8_t  fogNear;        /* EXPERIMENTAL (advRender): fog start idx, 0=auto */
    uint8_t  fogFar;         /* fog end idx, 0=auto */
    uint8_t  bloom;          /* bloom strength idx: 0 auto,1 off,2 low,3 high */
    uint8_t  skyObj[4];      /* skybox model slots: 0 none, else model idx+1
                                (SUN/MOON/OBJ A/OBJ B; gated by skyAssets) */
    uint8_t  dayCycle;       /* 0 off, 1 slow (10min), 2 med (2min), 3 fast (30s) */
    uint8_t  dayPhase;       /* persisted phase: 0..255 maps to 0..2pi */
    uint8_t  playerMode;     /* play-mode camera: 0 free fly, 1 first-person walk */
    uint8_t  fogCurve;       /* EXPERIMENTAL: fog falloff curve idx 0-3 -> 1/1.5/2/3 */
    uint8_t  pcfRad;         /* EXPERIMENTAL: shadow PCF radius idx 0-3 -> px */
    uint8_t  shRes;          /* EXPERIMENTAL: shadow map res idx 0-2 = 1/2/4K */
    uint8_t  msaa;           /* EXPERIMENTAL: 0 = MSAA 4x (default), 1 = off */
    uint8_t  pad[6];
} WSet;
#define WS_PHYS_SETTLE 0
#define WS_PHYS_RIGID  1
WSet *world_wset(void);          /* live record; edit in place */
void world_wset_dirty(void);     /* call after edits -> autosave notices */
void world_fog_params(float *near_, float *far_);   /* advRender-gated */
float world_fog_curve(void);   /* advRender-gated fog exponent (def 1.0) */
float world_pcf_rad(void);     /* advRender-gated shadow PCF radius px (def .5) */
int   world_shadow_res(void);  /* advRender-gated shadow map px (def 2048) */
int   world_msaa(void);        /* advRender-gated MSAA 4x (def on) */
float world_bloom_amt(void);                        /* advRender-gated */
/* day/night: advance the cycle; sun_dir returns the current light
 * direction (moon-reflected when the sun sets) and k the daylight 0..1 */
void world_day_tick(float dt);
void world_day_set(float t);           /* set phase (SET TIME / tests) */
void world_day_reload(void);           /* call on world load to re-seed */
v3   world_sun_dir(void);
float world_day_k(void);

/* play-mode sim lives in physics.c -> physics.h */

#endif
