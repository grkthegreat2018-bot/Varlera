/* app.h - shared application state (single global instance `A` in main.c). */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "math3d.h"
#include "world.h"

typedef enum { APP_EDIT, APP_PLAY, APP_MENU } AppMode;
typedef enum {
    M_IDLE,      /* nothing held */
    M_CLICK,     /* LMB held on block, awaiting drag threshold */
    M_PLACE,     /* dragging ghost from palette */
    M_ARMED,     /* asset armed; next world click places */
    M_MOVE,      /* dragging selected blocks */
    M_RESIZE,    /* dragging an axis handle */
    M_MARQUEE,   /* rubber-band select */
    M_LOOK,      /* RMB freecam look */
    M_PAN        /* MMB pan */
} DragMode;

typedef struct {
    GLFWwindow *win;
    double mx, my;               /* cursor px */
    int fbw, fbh;

    /* freecam */
    v3 camPos, camVel;
    float yaw, pitch, speed, fov;
    int invertY;
    float sens;                     /* mouse-look multiplier (def 1.0) */
    float autoSec;                  /* autosave interval s (0 = off, def 90) */
    m4 viewProj;

    AppMode app;
    DragMode drag;

    /* selection */
    uint8_t sel[MAX_BLOCKS];
    int nsel;

    /* edit payload */
    int armedAsset;              /* -1 = none; index into panel assets */
    int placeRot, ghostShapeMesh;
    v3 ghostPos, ghostSize;
    int overPanel;
    v3 grabPoint;
    float grabY, dyMove;
    v3 orig[MAX_BLOCKS];
    int resizeAxis, resizeSign;
    Block origBlock;
    float marqX0, marqY0, marqX1, marqY1;
    int hoverBlock, hoverAxis;   /* hoverAxis = axis*2 + (sign>0), -1 none */

    /* click-vs-drag tracking / dblclick / clipboard */
    float pressX, pressY;
    double rmbX, rmbY;
    v3 pressHit;
    int pressBlock;
    double lastClickT;
    int lastClickBlock;
    Block clip[MAX_BLOCKS];
    int nClip;

    /* panel */
    int panelOpen;
    int panelTab;                /* 0 shapes,1 paint,2 tex,3 world,4 models,5 props */
    float panelScroll;
    uint8_t catOpen[8];
    int curColor;                /* preset/custom index, -1 = picker */
    int paintTex;                /* active atlas texture id (0 = none) */
    int paintTexM;               /* TEXM_* mode|type bits for new/painted blocks */
    float paintAlpha;            /* block opacity (1 = opaque) */
    float paintCol[3];           /* active paint color */
    float hsv[3];                /* picker state */
    float customs[8][3];
    int nCustoms;
    int hoverAsset;
    int pickDrag;               /* 0 none, 1 = SV square drag, 2 = hue bar,
                                   3 = alpha slider, 4 = TEX slot reorder */

    double lastT, speedMsgT;
    uint16_t trigHeld;          /* play mode: bit per TRIG_* currently active */

    /* text input: TM_* focus modes share key/char dispatch */
    int textMode;               /* 0 off,1 search,2 console,3 rename,
                                   4 world name, 5 SAY/TITLE message */
    int msgSel;                 /* g_msgs slot edited by textMode 5 */
    char search[33];            /* SHAPES filter (non-empty = filtered view) */
    char cmdBuf[96]; int cmdLen;
    char renBuf[13];            /* textMode 3: block name being edited */
    char worldName[32];         /* world header name -> worlds/<name>.bw */

    /* modal editors: 0 none, 1 pixel-art tool, 2 sculpt mode */
    int modal;
    int artTex;                  /* texture id being edited */
    int artTool;                 /* 0 pen,1 erase,2 fill,3 pick,4 line,5 rect */
    int artLMB;

    /* borderless fullscreen (F11): saved windowed rect for restore */
    int winX, winY, winW, winH, fullscreen;
    int setDirty;                       /* settings changed -> save next frame */
    int guideOn;                        /* guide toasts (G toggles) */
    uint64_t guideSeen;                 /* TIP_* ids already shown */
} App;

extern App A;

void app_toggle_mode(void);   /* EDIT <-> PLAY (Tab / button) */

#endif /* APP_H */
