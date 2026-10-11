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

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK error %d at %s:%d\n", _r, __FILE__, __LINE__); exit(1); } } while (0)

typedef struct { float pos[2]; float col[4]; float uv[2]; float tex; } UIVert;
typedef struct { m4 model; float color[4]; uint32_t flags; uint32_t pad[3]; } Push;
typedef struct {
    m4 viewProj, iconVP, shadowVP;
    float camPos[4], lightDir[4], fogColor[4], fogParams[4];
    float texRect[RK_TEX_MAX][4];   /* u0,v0,tileW,tileH per texture id */
    float groundCol[4];             /* grid base color (world-settings tint) */
} UBO;

typedef struct { float x, y, w, h, r, g, b, a;
                 float u0, v0, u1, v1; int tex; } UIRect;

v3 rk_light_dir(void);
void rk_swapchain_recreate(void);

enum { K_GRID, K_WORLD, K_OVERLAY, K_GHOST, K_MESH, K_MLINES, K_LINES, K_UI, K_ICON };
typedef struct {
    int kind, mesh;
    uint32_t first, count;
    int px, py, pw, ph;
    float cr, cg, cb;
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
    VkPipeline pGrid, pWorld, pWorldInst, pGhostInst, pLinesInst, pShadow, pUI;
    VkPipeline pMeshLine;            /* non-instanced LINE_LIST (wireframe) */
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
    } fr[MAX_FRAMES];
    VkSemaphore renderDone[MAX_IMAGES];
    uint32_t renderDoneCount;

    VkBuffer vbuf; VkDeviceMemory vbufMem;
    VkDeviceSize meshOff[RK_MESH_COUNT + 1];
    uint32_t meshCount[RK_MESH_COUNT + 1];
    VkDeviceSize lineOff; uint32_t lineCount;

    /* baked custom-model meshes: CPU store + rebuilt GPU buffer */
    VkBuffer mbuf; VkDeviceMemory mbufMem;
    Vert mverts[256 * 1024]; int mvn;     /* model vert pool */
    VkDeviceSize modelOff[RK_MAX_MODELS];
    uint32_t modelCount[RK_MAX_MODELS];
    int nmodels;

    /* per-frame queues */
    QEnt q[MAX_Q];           int qn;
    RkInst instStage[MAX_INST]; int instN;
    UIRect uiQ[MAX_UI_QUADS];   int uiN;
    UIVert uiStage[MAX_UI_QUADS * 6]; int uiVn;
    Vert meshStage[MAX_DYN_VERTS]; int meshVn;   /* K_MESH streamed verts */

    UBO ubo;
    v3 focus;
    uint32_t fi, imgIdx;
    VkCommandBuffer cmd;
    int inFrame;
    int needResize;
    double t;
} RkG;

extern RkG G;                  /* defined in rk.c */

#define HOST_MEM (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

#endif /* RK_INT_H */