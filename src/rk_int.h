/* rk_int.h - renderer-internal state shared between rk.c (frame/draw
 * path) and rk_vk.c (device, swapchain, resources, uploads). */
#ifndef RK_INT_H
#define RK_INT_H

#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rk.h"
#include "math3d.h"
#include "meshgen.h"
#define MAX_FRAMES   2
#define MAX_IMAGES   8
#define MAX_UI_QUADS 16384
#define MAX_VERTS    24576
#define MAX_DYN_VERTS (256*1024)
#define MAX_INST     8192
#define MAX_Q        1024
#define SHADOW_SIZE  2048
#define BLOOM_M      5          /* bloom mip count (base = fb/2) */

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK error %d at %s:%d\n", _r, __FILE__, __LINE__); exit(1); } } while (0)

typedef struct { float pos[2]; float col[4]; float uv[2]; float tex; } UIVert;
typedef struct { m4 model; float color[4]; uint32_t flags; uint32_t pad[3]; } Push;
typedef struct {
    m4 viewProj, iconVP, shadowVP;
    float camPos[4], lightDir[4], fogColor[4], fogParams[4];
    float texRect[RK_TEX_MAX][4];   /* u0,v0,tileW,tileH per texture id */
    float texFps[RK_TEX_MAX];       /* flipbook fps per id (mode 3) */
    float groundCol[4];             /* grid base color (world-settings tint) */
} UBO;

typedef struct { float x, y, w, h, r, g, b, a;
                 float u0, v0, u1, v1; int tex; } UIRect;
typedef struct { float texel[2]; float a, b; } PostPC;

v3 rk_light_dir(void);
void rk_swapchain_recreate(void);
void rk_msaa_rebuild(void);      /* swapchain targets + pipelines at G.samples */
VkBuffer buf_create(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, VkDeviceMemory *outMem, void **mapped);

enum { K_GRID, K_WORLD, K_OVERLAY, K_GHOST, K_MESH, K_MLINES, K_LINES, K_UI, K_ICON, K_CASTER, K_SKY };
typedef struct {
    int kind, mesh;
    uint32_t first, count;
    int px, py, pw, ph;
    float cr, cg, cb;
    uint32_t flags;              /* K_ICON: extra RKIF_* bits (mat/tex) */
    float mm[16];                /* K_MESH model matrix */
} QEnt;

typedef struct {
    GLFWwindow *win;
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;

    VkSwapchainKHR swap;
    VkFormat surfFmt;
    VkColorSpaceKHR surfCS;
    VkExtent2D ext;
    uint32_t imgCount;
    VkImage imgs[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkImage msaaImg, depthImg;
    VkImageView msaaView, depthView;
    VkDeviceMemory msaaMem, depthMem;
    VkSampleCountFlagBits samples;
    VkSampleCountFlags    msaaSup;            /* device-supported mask */

    /* shadow map (fixed size, lives forever) */
    VkImage shadowImg;
    VkImageView shadowView;
    VkDeviceMemory shadowMem;
    VkSampler shadowSamp;

    /* texture atlas (fixed 1024x1024 RGBA, lives forever) */
    VkImage texImg;
    VkImageView texView;
    VkDeviceMemory texMem;
    VkSampler texSamp;

    VkDescriptorSetLayout setLayout0, setLayout1;
    VkDescriptorPool pool;
    VkDescriptorSet set1;
    VkPipelineLayout pipeLayout;
    VkPipeline pGrid, pWorld, pWorldInst, pGhostInst, pLinesInst, pShadow, pUI, pSky;
    VkPipeline pMeshLine;            /* non-instanced LINE_LIST (wireframe) */

    /* HDR post chain: scene -> hdrImg (msaa resolve or direct) -> bloom
       down/up mip chain -> composite(tonemap) -> swapchain -> LDR UI pass.
       hdrOk=0 falls back to surfFmt for the scene target (passthrough). */
    VkFormat hdrFmt;
    int hdrOk, bloomOk, bloomLive;
    float bloomAmt;                     /* composite mix (world-set override) */
    VkImage hdrImg;        VkImageView hdrView;  VkDeviceMemory hdrMem;
    VkImage bloomImg;      VkDeviceMemory bloomMem;
    VkImageView bloomMipView[BLOOM_M];
    VkImage uiDepthImg;    VkImageView uiDepthView; VkDeviceMemory uiDepthMem;
    VkDescriptorSetLayout postSetLayout;
    VkPipelineLayout postLayout;
    VkSampler postSamp;
    VkDescriptorSet postDS[BLOOM_M + 1];  /* 0: hdr+bloom0, i: bloom mip i-1 */
    VkPipeline pDown, pUp, pPost;         /* bloom down/up + composite */
    VkPipeline pIcon, pUIFlat;            /* 1x variants for the LDR UI pass */
    VkCommandPool cmdPool;

    struct {
        VkCommandBuffer cmd;
        VkFence fence;
        VkSemaphore acq;
        VkBuffer ubo;    VkDeviceMemory uboMem;  void *uboMap;
        VkBuffer inst;   VkDeviceMemory instMem; void *instMap;
        VkBuffer uib;    VkDeviceMemory uibMem;  void *uiMap;
        VkBuffer mesh;   VkDeviceMemory meshMem; void *meshMap;
        VkDescriptorSet set0;
        VkQueryPool qp;                     /* gpu pass timestamps */
        int tsRec;                          /* results pending */
    } fr[MAX_FRAMES];
    VkSemaphore renderDone[MAX_IMAGES];
    uint32_t renderDoneCount;

    VkBuffer vbuf; VkDeviceMemory vbufMem;
    VkDeviceSize meshOff[RK_MESH_COUNT + 1];
    uint32_t meshCount[RK_MESH_COUNT + 1];
    VkDeviceSize lineOff; uint32_t lineCount;

    /* baked custom-model meshes: CPU store + rebuilt GPU buffer.
     * Transient meshes (play-mode greedy clusters) allocate slots+verts
     * top-down: slot = --mtop, verts end at mvt and grow downward. They
     * never collide with dense persistent ids and drop for free. */
    VkBuffer mbuf; VkDeviceMemory mbufMem;
    Vert mverts[256 * 1024]; int mvn;     /* model vert pool (head) */
    VkDeviceSize modelOff[RK_MAX_MODELS];
    uint32_t modelCount[RK_MAX_MODELS];
    int nmodels;
    int mtop;                           /* next transient slot (down) */
    int mvt;                            /* transient vert tail (down) */

    /* per-frame queues */
    QEnt q[MAX_Q];           int qn;
    RkInst instStage[MAX_INST]; int instN;
    UIRect uiQ[MAX_UI_QUADS];   int uiN;
    UIVert uiStage[MAX_UI_QUADS * 6]; int uiVn;
    Vert meshStage[MAX_DYN_VERTS]; int meshVn;   /* K_MESH streamed verts */

    UBO ubo;
    v3 focus;
    v3 lightDir;                            /* dynamic sun/moon direction */
    float envSky[3], envGround[3];          /* preset bases (day-mul target) */
    int envStyle;
    float dayK;                             /* 0..1 daylight factor */
    m4 shView;                              /* light-space view (culling) */
    int shSize;                             /* shadow map px (runtime-resizable) */
    uint32_t fi, imgIdx;
    VkCommandBuffer cmd;
    int inFrame;
    int needResize;
    VkSampleCountFlagBits wantSamples;   /* pending MSAA change (0 = none) */
    double t;
    int hasTS;
    double tsPeriod;
    float gpuMs[4];                         /* shadow, main, post, total */

    /* screenshot readback: copy swapchain image -> staging buf this frame,
       write the .bmp next frame (fence guarantees completion) */
    char         shotPath[260];
    int          shotPend, shotW, shotH;
    VkBuffer     shotBuf;
    VkDeviceMemory shotMem;
    void        *shotMap;
    VkDeviceSize shotCap;
} RkG;

extern RkG G;                  /* defined in rk.c */

#define HOST_MEM (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

#endif /* RK_INT_H */