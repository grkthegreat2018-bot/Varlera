/* settings.c - settings.bw persistence: window rect, camera prefs, prof. */
#include "settings.h"
#include "app.h"
#include "prof.h"
#include "world.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define SET_MAGIC 0x54535742u   /* 'BWTS' */
#define SET_VER   3             /* v3 adds autoSec + sens */

typedef struct {
    uint32_t magic, ver;
    float    speed, fov;
    int32_t  invertY, profOn, fullscreen, winX, winY, winW, winH;
    int32_t  guideOff;
    uint64_t guideSeen;
    char     author[24];        /* v3 (pad-carved): local display name */
    uint8_t  pad[28];
    char     saveDir[384];      /* v2: user-chosen world save folder */
    float    autoSec;           /* v3: autosave seconds (0 = off) */
    float    sens;              /* v3: mouse-look multiplier */
} Settings;

static Settings s_cur;

void settings_load(void) {
    memset(&s_cur, 0, sizeof s_cur);
    FILE *f = fopen("settings.bw", "rb");
    if (!f) return;
    /* read only the bytes the file's version actually contains */
    if (fread(&s_cur, 8, 1, f) == 1 && s_cur.magic == SET_MAGIC) {
        size_t want = s_cur.ver == 1 ? offsetof(Settings, saveDir)
                    : s_cur.ver == 2 ? offsetof(Settings, autoSec)
                    : s_cur.ver == 3 ? sizeof s_cur : 0;
        if (want && (fseek(f, 0, SEEK_SET) == 0) &&
            fread(&s_cur, want, 1, f) == 1)
            s_cur.ver = SET_VER;
        else
            memset(&s_cur, 0, sizeof s_cur);
    } else memset(&s_cur, 0, sizeof s_cur);
    fclose(f);
    /* windowed placement applies before the window is created */
    if (s_cur.winW > 0) {
        A.winX = s_cur.winX; A.winY = s_cur.winY;
        A.winW = s_cur.winW; A.winH = s_cur.winH;
        A.fullscreen = s_cur.fullscreen;
    }
}

void settings_apply(void) {
    A.speed   = s_cur.speed > 0.0f ? s_cur.speed : 14.0f;
    A.fov     = s_cur.fov > 0.3f && s_cur.fov < 2.4f ? s_cur.fov : 1.05f;
    A.invertY = s_cur.invertY ? 1 : 0;
    A.guideOn = !s_cur.guideOff;
    A.guideSeen = s_cur.guideSeen;
    if (!!s_cur.profOn != prof_shown()) prof_toggle();
    worlds_set_dir(s_cur.saveDir);
    /* 0 in the file = pre-v3 (default 90); <0 = user-disabled autosave */
    A.autoSec = s_cur.autoSec > 0.0f ? s_cur.autoSec
              : s_cur.autoSec < 0.0f ? 0.0f : 90.0f;
    A.sens = s_cur.sens > 0.05f ? s_cur.sens : 1.0f;
    snprintf(A.author, sizeof A.author, "%s", s_cur.author);
}

void settings_save(void) {
    s_cur.magic = SET_MAGIC; s_cur.ver = SET_VER;
    s_cur.speed = A.speed; s_cur.fov = A.fov;
    s_cur.invertY = A.invertY;
    s_cur.profOn = prof_shown();
    s_cur.fullscreen = A.fullscreen;
    s_cur.winX = A.winX; s_cur.winY = A.winY;
    s_cur.winW = A.winW; s_cur.winH = A.winH;
    s_cur.guideOff = !A.guideOn;
    s_cur.guideSeen = A.guideSeen;
    snprintf(s_cur.saveDir, sizeof s_cur.saveDir, "%s", worlds_dir());
    s_cur.autoSec = A.autoSec;
    s_cur.sens = A.sens;
    snprintf(s_cur.author, sizeof s_cur.author, "%s", A.author);
    FILE *f = fopen("settings.bw", "wb");
    if (!f) return;
    fwrite(&s_cur, sizeof s_cur, 1, f);
    fclose(f);
}
