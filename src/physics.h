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
void world_reset_physics(void); /* restores pre-play transforms */

void world_snapshot_play(void); /* capture state before physics mutates it */
void world_restore_play(void);

#endif
