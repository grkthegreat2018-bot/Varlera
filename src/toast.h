/* toast.h - guide toast: cycling tips + first-time contextual tips.
 * Toggle via settings (guideOn); each tip id shows once until seen. */
#ifndef TOAST_H
#define TOAST_H

/* contextual tip ids (bit index into the persisted seen-set) */
enum {
    TIP_PLACE_BASIC, TIP_PLACE_RAMP, TIP_PLACE_SPECIAL, TIP_PLACE_MODEL,
    TIP_SCULPT, TIP_PLAY, TIP_FUSE, TIP_ARMED,
    TIP_COUNT
};

void toast_tip(int id);              /* show once per install (seen-set) */
void toast_say(const char *msg);     /* unconditional transient message */
void toast_title(const char *msg);   /* center-screen title card (ACT_TITLE) */
void toast_skip_fade(void);          /* jump card past fade-in (test hooks) */
void toast_frame(float dt);          /* advance fade/cycle timers */
void toast_draw(void);               /* call inside UI queue, before flush */
void hints_draw(void);               /* bottom hotkey strip (guideOn-gated) */

#endif
