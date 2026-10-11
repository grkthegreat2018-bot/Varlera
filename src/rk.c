/* rk.c - per-frame command recording + the deferred draw queue.
 * Device/swapchain/resource init lives in rk_vk.c; shared state
 * (G, queue types, helpers) is declared in rk_int.h. */
#include "rk_int.h"

RkG G;
/* ---------------- draw queue ---------------- */

static void qu(int kind, int mesh, uint32_t first, uint32_t count) {
    if (G.qn < MAX_Q)
        G.q[G.qn++] = (QEnt){kind, mesh, first, count, 0,0,0,0, 0,0,0};
}

void rk_draw_grid(void) { qu(K_GRID, 0, 0, 0); }

static uint32_t push_insts(const RkInst *inst, uint32_t n) {
    uint32_t first = (uint32_t)G.instN;
    for (uint32_t i = 0; i < n && G.instN < MAX_INST; i++)
        G.instStage[G.instN++] = inst[i];
    return first;
}

void rk_world(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_WORLD, m, push_insts(inst, n), n);
}
void rk_overlay(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_OVERLAY, m, push_insts(inst, n), n);
}
void rk_ghost(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_GHOST, m, push_insts(inst, n), n);
}
void rk_lines(const RkInst *inst, uint32_t n) {
    qu(K_LINES, 0, push_insts(inst, n), n);
}
void rk_mesh(const Vert *v, uint32_t n, const float model[16]) {
    uint32_t first = (uint32_t)G.meshVn;
    uint32_t room = MAX_DYN_VERTS - (uint32_t)G.meshVn;
    if (n > room) n = room;
    if (!n) return;
    memcpy(G.meshStage + G.meshVn, v, (size_t)n * sizeof(Vert));
    G.meshVn += (int)n;
    if (G.qn < MAX_Q) {
        QEnt *e = &G.q[G.qn++];
        *e = (QEnt){K_MESH, 0, first, n, 0,0,0,0, 0,0,0};
        memcpy(e->mm, model, sizeof e->mm);
    }
}
void rk_meshlines(const Vert *v, uint32_t n, const float model[16],
                  float r, float g, float b, float a) {
    uint32_t first = (uint32_t)G.meshVn;
    uint32_t room = MAX_DYN_VERTS - (uint32_t)G.meshVn;
    if (n > room) n = room;
    if (!n) return;
    memcpy(G.meshStage + G.meshVn, v, (size_t)n * sizeof(Vert));
    G.meshVn += (int)n;
    if (G.qn < MAX_Q) {
        QEnt *e = &G.q[G.qn++];
        *e = (QEnt){K_MLINES, 0, first, n, 0,0,0,0, r,g,b};
        memcpy(e->mm, model, sizeof e->mm);
        e->pw = (int)(a * 1000.0f);      /* alpha rides in pw */
    }
}
/* icon framing: yaw/pitch per mesh so every shape's signature face shows.
   The icon camera sits on the +x+y+z diagonal; yaw -0.79 turns a mesh's
   x-side profile square to it, steeper rx tilts the top into view. */
static void icon_angles(int mesh, float *rx, float *ry) {
    *rx = -0.55f; *ry = 0.75f;
    switch (mesh) {
    case RK_MESH_WEDGE:
    case RK_MESH_STAIR:
    case RK_MESH_WHEEL:   *ry = -0.79f; break;       /* side profile */
    case RK_MESH_TORUS:   *rx = -1.15f; break;       /* see the hole */
    case RK_MESH_CONE:
    case RK_MESH_PYRAMID: *rx = -0.30f; break;       /* silhouette */
    case RK_MESH_DOME:    *rx = -0.35f; break;
    case RK_MESH_CORNERW: *ry = 2.35f; break;        /* apex corner out */
    case RK_MESH_CORSTAIR:*ry = -0.40f; *rx = -0.62f; break;
    }
}

/* panel icon: per-shape readable angle, scaled to the asset's real
   proportions (largest dimension fills the cell). size=NULL -> unit. */
void rk_draw_icon(RkMesh m, int px, int py, int pw, int ph,
                  float r, float g, float b, const float size[3]) {
    if (G.qn >= MAX_Q) return;
    QEnt *e = &G.q[G.qn++];
    *e = (QEnt){K_ICON, m, 0, 0, px, py, pw, ph, r, g, b, {0}};
    float mx = size ? fmaxf(size[0], fmaxf(size[1], size[2])) : 1.0f;
    float k = mx > 1e-5f ? 0.62f / mx : 0.62f;
    v3 sc = size ? v3_make(size[0]*k, size[1]*k, size[2]*k)
               : v3_make(0.62f, 0.62f, 0.62f);
    float rx, ry; icon_angles(m, &rx, &ry);
    m4_trs(e->mm, v3_make(0,0,0), sc, rx, ry, 0);
}

void rk_ui_rect(float px, float py, float pw, float ph, float r, float g, float b, float a) {
    if (G.uiN < MAX_UI_QUADS)
        G.uiQ[G.uiN++] = (UIRect){px, py, pw, ph, r, g, b, a, 0,0,0,0, -1};
}

/* textured UI quad: samples the atlas region [u0,v0]-[u1,v1] */
void rk_ui_texrect(float px, float py, float pw, float ph,
                   float u0, float v0, float u1, float v1) {
    if (G.uiN < MAX_UI_QUADS)
        G.uiQ[G.uiN++] = (UIRect){px, py, pw, ph, 1,1,1,1, u0, v0, u1, v1, 1};
}

void rk_ui_flush(void) {
    /* convert queued rects to staged verts, record a UI queue entry */
    uint32_t first = (uint32_t)G.uiVn;
    int n = 0;
    for (int i = 0; i < G.uiN && G.uiVn + 6 <= MAX_UI_QUADS*6; i++) {
        UIRect *q = &G.uiQ[i];
        float x0 = q->x / G.ext.width * 2.0f - 1.0f;
        float x1 = (q->x + q->w) / G.ext.width * 2.0f - 1.0f;
        float y0 = q->y / G.ext.height * 2.0f - 1.0f;
        float y1 = (q->y + q->h) / G.ext.height * 2.0f - 1.0f;
        float c[4] = {q->r, q->g, q->b, q->a};
        float us[6] = {q->u0, q->u1, q->u1, q->u0, q->u1, q->u0};
        float vs[6] = {q->v0, q->v0, q->v1, q->v0, q->v1, q->v1};
        float xs[6] = {x0, x1, x1, x0, x1, x0};
        float ys[6] = {y0, y0, y1, y0, y1, y1};
        UIVert *o = G.uiStage + G.uiVn;
        for (int k = 0; k < 6; k++) {
            o[k].pos[0] = xs[k]; o[k].pos[1] = ys[k];
            memcpy(o[k].col, c, 16);
            o[k].uv[0] = us[k]; o[k].uv[1] = vs[k];
            o[k].tex = (float)q->tex;
        }
        G.uiVn += 6; n += 6;
    }
    G.uiN = 0;
    if (n) qu(K_UI, 0, first, (uint32_t)n);
}

/* ---------------- frame ---------------- */

static void img_barrier(VkImage img, VkImageLayout oldL, VkImageLayout newL,
                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAcc,
                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAcc,
                        VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAcc;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAcc;
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(G.cmd, &dep);
}

int rk_begin_frame(void) {
    if (G.needResize) rk_swapchain_recreate();
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(G.win, &fw, &fh);
    if (fw == 0 || fh == 0) return 0;
    if ((uint32_t)fw != G.ext.width || (uint32_t)fh != G.ext.height)
        rk_swapchain_recreate();
    if (G.ext.width == 0) return 0;

    VKC(vkWaitForFences(G.dev, 1, &G.fr[G.fi].fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(G.dev, 1, &G.fr[G.fi].fence));

    /* this frame slot's gpu work is finished -> timestamp results ready */
    if (G.hasTS && G.fr[G.fi].tsRec) {
        uint64_t ts[3] = {0};
        if (vkGetQueryPoolResults(G.dev, G.fr[G.fi].qp, 0, 3, sizeof ts, ts,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)
            == VK_SUCCESS) {
            G.gpuMs[0] = (float)((ts[1]-ts[0]) * G.tsPeriod / 1e6);
            G.gpuMs[1] = (float)((ts[2]-ts[1]) * G.tsPeriod / 1e6);
            G.gpuMs[2] = (float)((ts[2]-ts[0]) * G.tsPeriod / 1e6);
        }
        G.fr[G.fi].tsRec = 0;
    }

    VkResult r = vkAcquireNextImageKHR(G.dev, G.swap, UINT64_MAX, G.fr[G.fi].acq, VK_NULL_HANDLE, &G.imgIdx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { rk_swapchain_recreate(); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VKC(r);

    G.cmd = G.fr[G.fi].cmd;
    VKC(vkResetCommandBuffer(G.cmd, 0));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(G.cmd, &bbi));

    G.qn = 0; G.instN = 0; G.uiN = 0; G.uiVn = 0; G.meshVn = 0;
    G.t = glfwGetTime();
    G.inFrame = 1;
    return 1;
}

static void bind_sets_and_mesh(void) {
    VkDescriptorSet ds[2] = {G.fr[G.fi].set0, G.set1};
    vkCmdBindDescriptorSets(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipeLayout,
        0, 2, ds, 0, NULL);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.vbuf, &off);
}

/* model meshes live in a separate rebuilt buffer */
static int mesh_span(int mesh, VkDeviceSize *off, uint32_t *cnt) {
    if (mesh >= RK_MESH_BASE_MODEL) {
        int m = mesh - RK_MESH_BASE_MODEL;
        if (m >= G.nmodels) return 0;
        *off = G.modelOff[m]; *cnt = G.modelCount[m];
        return G.mbuf != VK_NULL_HANDLE;
    }
    *off = G.meshOff[mesh]; *cnt = G.meshCount[mesh];
    return 1;
}
static void bind_mesh_buf(int mesh) {
    VkBuffer b = (mesh >= RK_MESH_BASE_MODEL) ? G.mbuf : G.vbuf;
    VkDeviceSize off = 0;
    if (b) vkCmdBindVertexBuffers(G.cmd, 0, 1, &b, &off);
}

static void bind_inst(void) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(G.cmd, 1, 1, &G.fr[G.fi].inst, &off);
}

static void push_pc(const m4 model, const float color[4], uint32_t flags) {
    Push pc; memcpy(pc.model, model, sizeof(m4));
    memcpy(pc.color, color, sizeof(float)*4); pc.flags = flags;
    vkCmdPushConstants(G.cmd, G.pipeLayout,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
}

void rk_end_frame(void) {
    /* stage instance + ui data for this frame */
    if (G.hasTS) {
        vkCmdResetQueryPool(G.cmd, G.fr[G.fi].qp, 0, 4);
        vkCmdWriteTimestamp(G.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            G.fr[G.fi].qp, 0);
        G.fr[G.fi].tsRec = 1;
    }
    if (G.instN)
        memcpy(G.fr[G.fi].instMap, G.instStage, (size_t)G.instN * sizeof(RkInst));
    if (G.uiVn)
        memcpy(G.fr[G.fi].uiMap, G.uiStage, (size_t)G.uiVn * sizeof(UIVert));
    if (G.meshVn)
        memcpy(G.fr[G.fi].meshMap, G.meshStage, (size_t)G.meshVn * sizeof(Vert));

    /* ---- shadow pass (always runs so the map is in READ layout for sampling) ---- */
    {
        img_barrier(G.shadowImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo dep = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        dep.imageView = G.shadowView; dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        dep.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = (VkRect2D){{0,0},{SHADOW_SIZE, SHADOW_SIZE}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &dep;
        vkCmdBeginRendering(G.cmd, &ri);
        VkViewport vp = {0, 0, SHADOW_SIZE, SHADOW_SIZE, 0.0f, 1.0f};
        vkCmdSetViewport(G.cmd, 0, 1, &vp);
        VkRect2D sc = {{0,0},{SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetScissor(G.cmd, 0, 1, &sc);
        vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pShadow);
        bind_sets_and_mesh();
        bind_inst();
        for (int i = 0; i < G.qn; i++) {
            QEnt *e = &G.q[i];
            if (e->kind != K_WORLD) continue;
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) continue;
            bind_mesh_buf(e->mesh);
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
        }
        vkCmdEndRendering(G.cmd);
        if (G.hasTS) vkCmdWriteTimestamp(G.cmd,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, G.fr[G.fi].qp, 1);
        img_barrier(G.shadowImg, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    /* ---- main pass ---- */
    img_barrier(G.imgs[G.imgIdx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (G.msaaImg)
        img_barrier(G.msaaImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    img_barrier(G.depthImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    int msaa = G.samples != VK_SAMPLE_COUNT_1_BIT;
    VkRenderingAttachmentInfo col = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = msaa ? G.msaaView : G.views[G.imgIdx];
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (msaa) {
        col.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        col.resolveImageView = G.views[G.imgIdx];
        col.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    col.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    col.clearValue.color.float32[0] = G.ubo.fogColor[0];
    col.clearValue.color.float32[1] = G.ubo.fogColor[1];
    col.clearValue.color.float32[2] = G.ubo.fogColor[2];
    col.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo dep = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = G.depthView; dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dep.clearValue.depthStencil.depth = 0.0f;   /* reversed-Z far */

    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0,0}, G.ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &col;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(G.cmd, &ri);

    VkViewport fullvp = {0, 0, (float)G.ext.width, (float)G.ext.height, 0.0f, 1.0f};
    VkRect2D fullsc = {{0,0}, G.ext};
    vkCmdSetViewport(G.cmd, 0, 1, &fullvp);
    vkCmdSetScissor(G.cmd, 0, 1, &fullsc);

    for (int i = 0; i < G.qn; i++) {
        QEnt *e = &G.q[i];
        switch (e->kind) {
        case K_GRID: {
            m4 m;
            v3 t = v3_make(snapf(G.ubo.camPos[0], 1.0f), -0.02f, snapf(G.ubo.camPos[2], 1.0f));
            m4_trs(m, t, v3_make(360, 1, 360), 0, 0, 0);
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pGrid);
            bind_sets_and_mesh();
            float white[4] = {1,1,1,1};
            push_pc(m, white, 0);
            vkCmdDraw(G.cmd, G.meshCount[MESHGEN_PLANE], 1, (uint32_t)G.meshOff[MESHGEN_PLANE], 0);
            break; }
        case K_WORLD:
        case K_OVERLAY: {
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) break;
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorldInst);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            bind_inst();
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
            break; }
        case K_GHOST: {
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) break;
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pGhostInst);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            bind_inst();
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
            break; }
        case K_MESH:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorld);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].mesh, &(VkDeviceSize){0});
            {   float c[4] = {1,1,1,1};
    push_pc(e->mm, c, RKIF_VCOL | RKIF_TEXM(1)); }
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_MLINES:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pMeshLine);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].mesh, &(VkDeviceSize){0});
            {   float c[4] = {e->cr, e->cg, e->cb, (float)e->pw / 1000.0f};
                push_pc(e->mm, c, RKIF_UNLIT | RKIF_NOFOG); }
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_LINES:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pLinesInst);
            bind_sets_and_mesh();
            bind_mesh_buf(0);
            bind_inst();
            vkCmdDraw(G.cmd, G.lineCount, e->count, (uint32_t)G.lineOff, e->first);
            break;
        case K_UI:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pUI);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].uib, &(VkDeviceSize){0});
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_ICON: {
            VkViewport vp = {(float)e->px, (float)e->py, (float)e->pw, (float)e->ph, 0, 1};
            vkCmdSetViewport(G.cmd, 0, 1, &vp);
            VkRect2D sc = {{e->px, e->py}, {(uint32_t)e->pw, (uint32_t)e->ph}};
            vkCmdSetScissor(G.cmd, 0, 1, &sc);
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorld);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            float c[4] = {e->cr, e->cg, e->cb, 1};
            uint32_t icol = e->mesh >= RK_MESH_BASE_MODEL ? RKF_ICON|RKIF_VCOL : RKF_ICON;
            push_pc(e->mm, c, icol);
            VkDeviceSize io; uint32_t ic;
            if (mesh_span(e->mesh, &io, &ic)) vkCmdDraw(G.cmd, ic, 1, (uint32_t)io, 0);
            vkCmdSetViewport(G.cmd, 0, 1, &fullvp);
            vkCmdSetScissor(G.cmd, 0, 1, &fullsc);
            break; }
        }
    }

    vkCmdEndRendering(G.cmd);
    if (G.hasTS) vkCmdWriteTimestamp(G.cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, G.fr[G.fi].qp, 2);
    img_barrier(G.imgs[G.imgIdx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT);
    VKC(vkEndCommandBuffer(G.cmd));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &G.fr[G.fi].acq;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &G.cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &G.renderDone[G.imgIdx];
    VKC(vkQueueSubmit(G.queue, 1, &si, G.fr[G.fi].fence));

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &G.renderDone[G.imgIdx];
    pi.swapchainCount = 1; pi.pSwapchains = &G.swap;
    pi.pImageIndices = &G.imgIdx;
    VkResult r = vkQueuePresentKHR(G.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) G.needResize = 1;
    else if (r != VK_SUCCESS) VKC(r);

    G.fi = (G.fi + 1) % MAX_FRAMES;
    G.inFrame = 0;
}

v3 rk_light_dir(void) { return v3_norm(v3_make(0.72f, 0.55f, 0.30f)); }

void rk_set_camera(const float vp[16], const float eye[3], const float focus[3]) {
    memcpy(G.ubo.viewProj, vp, sizeof(m4));
    memcpy(G.ubo.camPos, eye, sizeof(float)*3);
    G.ubo.camPos[3] = (float)G.t;          /* w = time (anim/spin) */
    memcpy(&G.focus, focus, sizeof(v3));

    /* directional shadow ortho, centered on snapped focus for stability */
    v3 f = v3_make(snapf(focus[0], 2.0f), focus[1], snapf(focus[2], 2.0f));
    v3 l = rk_light_dir();
    v3 le = v3_add(f, v3_scale(l, 100.0f));   /* eye on the light side, above scene */
    m4 v, p;
    m4_look_at(v, le, f, v3_make(0, 1, 0));
    float R = 60.0f;
    m4_ortho(p, -R, R, -R, R, 1.0f, 220.0f);
    m4_mul(G.ubo.shadowVP, p, v);

    memcpy(G.fr[G.fi].uboMap, &G.ubo, sizeof(UBO));
}

void rk_extent(int *w, int *h) { *w = (int)G.ext.width; *h = (int)G.ext.height; }

void rk_query_stats(RkStats *s) {
    s->draws = G.qn; s->insts = G.instN;
    s->uiVerts = G.uiVn; s->meshVerts = G.meshVn; s->models = G.nmodels;
}

int rk_gpu_times(float out[3]) {
    if (!G.hasTS || G.gpuMs[2] <= 0.0f) return 0;
    memcpy(out, G.gpuMs, sizeof G.gpuMs);
    return 1;
}