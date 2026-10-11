/* settings.c - settings.bw persistence: window rect, camera prefs, prof. */
#include "settings.h"
#include "app.h"
#include "prof.h"
#include <stdio.h>
#include <string.h>

#define SET_MAGIC 0x54535742u   /* 'BWTS' */
#define SET_VER   1

typedef struct {
    uint32_t magic, ver;
    float    speed, fov;
    int32_t  invertY, profOn, fullscreen, winX, winY, winW, winH;
    int32_t  guideOff;
    uint64_t guideSeen;
    uint8_t  pad[52];
} Settings;

static Settings s_cur;

void settings_load(void) {
    memset(&s_cur, 0, sizeof s_cur);
    FILE *f = fopen("settings.bw", "rb");
    if (!f) return;
    if (fread(&s_cur, sizeof s_cur, 1, f) != 1 ||
        s_cur.magic != SET_MAGIC || s_cur.ver != SET_VER) {
        memset(&s_cur, 0, sizeof s_cur);
    }
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
    FILE *f = fopen("settings.bw", "wb");
    if (!f) return;
    fwrite(&s_cur, sizeof s_cur, 1, f);
    fclose(f);
}
