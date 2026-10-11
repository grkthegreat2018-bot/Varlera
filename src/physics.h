/* physics.h - play-mode simulation: gravity settle, script actions,
 * play snapshot/restore. World data lives in world.c; this is the
 * per-frame runtime that mutates it while A.app == APP_PLAY. */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdint.h>

void world_physics(float dt, uint16_t trigHeld);
void world_reset_physics(void); /* restores pre-play transforms */

void world_snapshot_play(void); /* capture state before physics mutates it */
void world_restore_play(void);

#endif
