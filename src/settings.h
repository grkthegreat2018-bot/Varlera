/* settings.h - persisted user preferences (settings.bw). */
#ifndef SETTINGS_H
#define SETTINGS_H

void settings_load(void);    /* early: reads windowed rect + fullscreen flag */
void settings_apply(void);   /* after cam_init: speed/fov/invertY/profOn */
void settings_save(void);    /* shutdown + on any setting change */

#endif
