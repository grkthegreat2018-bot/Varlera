/* app.h - shared application state (single global instance `A` in main.c). */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "math3d.h"
#include "world.h"

typedef enum { APP_EDIT, APP_PLAY } AppMode;
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
    float yaw, pitch, speed;
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
    int panelTab;                /* 0 shapes, 1 paint, 2 world */
    float panelScroll;
    uint8_t catOpen[8];
    int curColor;                /* preset/custom index, -1 = picker */
    int paintTex;                /* active texture pattern 0-15 */
    float paintCol[3];           /* active paint color */
    float hsv[3];                /* picker state */
    float customs[8][3];
    int nCustoms;
    int hoverAsset;
    int pickDrag;               /* 0 none, 1 = SV square drag, 2 = hue bar drag */

    double lastT, speedMsgT;
    uint16_t trigHeld;          /* play mode: bit per TRIG_* currently active */
} App;

extern App A;

void app_toggle_mode(void);   /* EDIT <-> PLAY (Tab / button) */

#endif /* APP_H */
