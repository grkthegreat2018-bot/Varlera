/* physics.h - play-mode simulation: gravity settle, script actions,
 * play snapshot/restore. World data lives in world.c; this is the
 * per-frame runtime that mutates it while A.app == APP_PLAY. */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdint.h>
#include "math3d.h"   /* v3 */

void world_physics(float dt, uint16_t trigHeld);

/* play-mode LMB block drag (BW-style). grab on press, move stores the
 * cursor ray each frame (applied inside world_physics), release drops.
 * Disabled by WSet.dragOff; BF_LOCKED/NOPHYS blocks can't be grabbed. */
int  world_drag_grab(v3 ro, v3 rd);
void world_drag_move(v3 ro, v3 rd);
void world_drag_release(void);
int  world_dragging(void);
/* walk-mode player state (0 when player inactive) */
int  world_player_hp(void);
int  world_player_score(void);
int  world_player_taken(int i);   /* pickup already collected this session */
void world_reset_physics(void); /* restores pre-play transforms */

void world_snapshot_play(void); /* capture state before physics mutates it */
void world_restore_play(void);

/* greedy mesh merge: at play entry, settled script-free touching block
 * clusters bake into transient meshes (one draw each). Members stay in
 * g_blocks for support/pick but skip draw+settle via BF_MERGED; a script
 * touch, drag grab, or delete dissolves its cluster back to blocks. */
int  world_merged_count(void);
void world_merged_draw(void);

/* walk-mode player (WSet.playerMode): first-person kinematic box vs block
 * AABBs. enter/exit from app_toggle_mode; tick replaces cam_frame. */
int  world_player_active(void);
void world_player_enter(void);
void world_player_exit(void);
void world_player_tick(float dt);

#endif
