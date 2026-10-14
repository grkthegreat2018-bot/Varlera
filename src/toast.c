/* toast.c - guide toasts: rotating tips + one-shot contextual tips.
 * When A.guideOn: an idle cycler surfaces TIPS every ~10s; toast_tip(id)
 * fires once per install via A.guideSeen bitmask (settings.bw). */
#include "toast.h"
#include "app.h"
#include "font.h"
#include "panel.h"
#include "rk.h"
#include <stdio.h>
#include <string.h>

#define TOAST_HOLD 4.5f
#define TOAST_FADE 0.35f
#define TCARD_HOLD 3.2f
#define TCARD_FADE 0.6f
#define CYCLE_EVERY 10.0f

static const char *TIPS[] = {
    "RMB-drag looks, WASD+QE flies, wheel zooms to cursor",
    "Drag a shape onto the world - it stays armed for more clicks",
    "LMB selects, shift adds, drag on empty space marquees",
    "ctrl+B fuses a selection into a reusable model",
    "TAB toggles PLAY - gravity and block scripts come alive",
    "Select a block, then set TRIG/ACT in the docked menu",
    "ctrl+wheel changes fly speed; F focuses the selection",
    "RMB on the world eyedrops color + texture",
    "The TEX tab's art tool paints new atlas textures",
    "SCULPT (top-left) carves models vertex-by-vertex",
    "- / = adjust FOV, Y inverts look, F11 toggles fullscreen",
    "F3 shows the profiler; F12 saves a screenshot",
};
#define N_TIPS ((int)(sizeof TIPS / sizeof TIPS[0]))

/* first-time contextual tips, indexed by TIP_* */
static const char *TIP_MSG[TIP_COUNT] = {
    "Basic shape placed - drag stays armed, RMB disarms",
    "Ramps snap to the block lattice; R/T rotates them",
    "Special shapes shine with textures - try the TEX tab",
    "Models are fused groups - ctrl+U unfuses back to blocks",
    "Sculpt mode: LMB grabs verts, short RMB splits edges",
    "Play mode: TRIG keys fire block actions; Esc returns",
    "Fused! Find it under MODELS - place it like any block",
    "Armed: each world click places; wheel rotates the ghost",
};

static char  s_msg[160];
static float s_t           = -1.0f;   /* <0 idle, else time since show */
static float s_cycleT;
static int   s_cycleI;

void toast_tip(int id) {
    if (!A.guideOn || id < 0 || id >= TIP_COUNT) return;
    uint64_t bit = 1ull << id;
    if (A.guideSeen & bit) return;
    A.guideSeen |= bit;
    A.setDirty = 1;
    toast_say(TIP_MSG[id]);
}

void toast_say(const char *msg) {
    snprintf(s_msg, sizeof s_msg, "%s", msg);
    s_t = 0.0f;
}

/* ACT_TITLE channel: separate from toasts so a SAY can run under a card */
static char  s_tmsg[160];
static float s_tcard = -1.0f;

void toast_title(const char *msg) {
    snprintf(s_tmsg, sizeof s_tmsg, "%s", msg);
    s_tcard = 0.0f;
}

void toast_skip_fade(void) { if (s_tcard >= 0.0f) s_tcard = TCARD_FADE; }

void toast_frame(float dt) {
    if (s_t >= 0.0f) {
        s_t += dt;
        if (s_t > TOAST_HOLD + TOAST_FADE) s_t = -1.0f;
    }
    if (s_tcard >= 0.0f) {
        s_tcard += dt;
        if (s_tcard > TCARD_HOLD + TCARD_FADE * 2.0f) s_tcard = -1.0f;
    }
    if (A.guideOn && s_t < 0.0f) {          /* idle cycler */
        s_cycleT += dt;
        if (s_cycleT > CYCLE_EVERY) {
            s_cycleT = 0.0f;
            toast_say(TIPS[s_cycleI]);
            s_cycleI = (s_cycleI + 1) % N_TIPS;
        }
    } else s_cycleT = 0.0f;
}

/* title card: gold-framed scrim band across the upper screen —
 * separate entry so it draws even when no toast is showing */
static void title_draw(void) {
    if (s_tcard < 0.0f || !s_tmsg[0]) return;
    float ta = 1.0f;
    if (s_tcard < TCARD_FADE) ta = s_tcard / TCARD_FADE;
    else if (s_tcard > TCARD_HOLD + TCARD_FADE)
        ta = (TCARD_HOLD + TCARD_FADE * 2.0f - s_tcard) / TCARD_FADE;
    int sc = 4;
    float tw = (float)ui_text_w(s_tmsg, sc);
    if (tw > (float)A.fbw - 96.0f) {          /* long text shrinks */
        sc = tw > ((float)A.fbw - 96.0f) * 1.5f ? 2 : 3;
        tw = (float)ui_text_w(s_tmsg, sc);
    }
    float ty = (float)A.fbh * 0.30f;
    /* stop the scrim at the panel edge so it doesn't slab over the UI */
    float w = (!A.modal && A.app != APP_MENU) ? panel_x() : (float)A.fbw;
    rk_ui_rect(0, ty - 14, w, 58.0f, 0, 0, 0, 0.42f * ta);
    rk_ui_rect(0, ty - 14, w, 2.0f, 0.95f, 0.75f, 0.30f, 0.7f * ta);
    rk_ui_rect(0, ty + 42, w, 2.0f, 0.95f, 0.75f, 0.30f, 0.7f * ta);
    ui_text((float)A.fbw * 0.5f - tw * 0.5f, ty, s_tmsg, sc,
            1.0f, 0.88f, 0.55f, ta);
}

void toast_draw(void) {
    title_draw();
    if (s_t < 0.0f || !s_msg[0]) return;
    float a = 1.0f;
    if (s_t < TOAST_FADE) a = s_t / TOAST_FADE;
    else if (s_t > TOAST_HOLD) a = (TOAST_HOLD + TOAST_FADE - s_t) / TOAST_FADE;
    int sc = 1;
    float tw = (float)ui_text_w(s_msg, sc);
    float x = (float)A.fbw * 0.5f - tw * 0.5f - 12.0f;
    float y = (float)A.fbh - 64.0f;
    rk_ui_rect(x, y, tw + 24.0f, 22.0f, 0.10f, 0.11f, 0.14f, 0.88f * a);
    rk_ui_rect(x, y, tw + 24.0f, 2.0f, 0.95f, 0.75f, 0.30f, 0.9f * a);
    ui_text(x + 12, y + 7, s_msg, sc, 1.0f, 0.96f, 0.85f, a);
}

/* context-aware hotkey strip pinned to the bottom edge (guide G toggles) */
void hints_draw(void) {
    if (!A.guideOn || A.modal) return;
    const char *s;
    if (A.textMode)                                   s = "type + Enter commits | Esc cancels";
    else if (A.app == APP_PLAY)                       s = "Esc edit | LMB drag blocks | script keys SPACE W A S D E Q | CLICK";
    else if (A.drag == M_ARMED || A.drag == M_PLACE)  s = "LMB place | wheel rotate ghost | RMB/Esc disarm";
    else if (A.drag == M_MOVE)                        s = "release drops | PgUp/PgDn lift | Esc cancels";
    else if (A.nsel)                                  s = "ctrl+C/X/V clip | ctrl+D dup | ctrl+B fuse | R/T rot | arrows nudge | Del";
    else                                              s = "LMB select | shift multi | drag marquee | F3 prof | F12 shot | TAB play";
    int sc = 1;
    float tw = (float)ui_text_w(s, sc);
    float x = (float)A.fbw * 0.5f - tw * 0.5f - 12.0f;
    float y = (float)A.fbh - 30.0f;
    rk_ui_rect(x, y, tw + 24.0f, 20.0f, 0.08f, 0.09f, 0.11f, 0.82f);
    ui_text(x + 12, y + 6, s, sc, 0.72f, 0.76f, 0.84f, 1);
}
