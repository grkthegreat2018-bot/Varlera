/* rk_vk.c - Vulkan device/swapchain/pipeline init, image+buffer
 * resources, texture atlas + model-mesh uploads. Frame path lives
 * in rk.c; shared state is in rk_int.h. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif
#include "rk_int.h"
/* ---------------- memory & buffer helpers ---------------- */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(G.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

static VkDeviceMemory mem_alloc(VkDeviceSize size, uint32_t bits, VkMemoryPropertyFlags props) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = mem_type(bits, props);
    VkDeviceMemory m;
    VKC(vkAllocateMemory(G.dev, &ai, NULL, &m));
    return m;
}


VkBuffer buf_create(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props, VkDeviceMemory *outMem, void **mapped) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    VKC(vkCreateBuffer(G.dev, &bi, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(G.dev, buf, &req);
    *outMem = mem_alloc(req.size, req.memoryTypeBits, props);
    VKC(vkBindBufferMemory(G.dev, buf, *outMem, 0));
    if (mapped) VKC(vkMapMemory(G.dev, *outMem, 0, size, 0, mapped));
    return buf;
}

static void upload_vertices(const Vert *verts, int nv) {
    VkDeviceSize bytes = (VkDeviceSize)nv * sizeof(Vert);
    void *map;
    VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, verts, bytes);
    G.vbuf = buf_create(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.vbufMem, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkBufferCopy cp = {0, 0, bytes};
    vkCmdCopyBuffer(c, stag, G.vbuf, 1, &cp);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
}


static Vert s_mpool[MAX_VERTS];

static void build_mesh_data(void) {
    uint32_t off[MESHGEN_TOTAL], cnt[MESHGEN_TOTAL], lo, lc;
    int nv = meshgen_build_all(s_mpool, MAX_VERTS, off, cnt, &lo, &lc);
    for (int i = 0; i < MESHGEN_TOTAL; i++) {
        G.meshOff[i] = off[i];
        G.meshCount[i] = cnt[i];
    }
    G.lineOff = lo;
    G.lineCount = lc;
    upload_vertices(s_mpool, nv);
}

/* ---------------- texture atlas ---------------- */

void rk_tex_fps(int id, float fps) {
    if (id < 0 || id >= RK_TEX_MAX) return;
    G.ubo.texFps[id] = fps;
}

void rk_tex_rect(int id, float u0, float v0, float tw, float th) {
    if (id < 0 || id >= RK_TEX_MAX) return;
    G.ubo.texRect[id][0] = u0; G.ubo.texRect[id][1] = v0;
    G.ubo.texRect[id][2] = tw; G.ubo.texRect[id][3] = th;
}

static int s_texInit;

void rk_tex_upload(const uint8_t *rgba) {
    vkDeviceWaitIdle(G.dev);              /* texture edits are rare */
    VkDeviceSize bytes = (VkDeviceSize)RK_TEX_ATLAS * RK_TEX_ATLAS * 4;
    void *map; VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, rgba, bytes);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = s_texInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = G.texImg;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = s_texInit ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {0};
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){RK_TEX_ATLAS, RK_TEX_ATLAS, 1};
    vkCmdCopyBufferToImage(c, stag, G.texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
    s_texInit = 1;
}

/* ---------------- baked model meshes ---------------- */

#define MV_CAP ((int)(sizeof G.mverts / sizeof(Vert)))

void rk_models_reset(void) {
    G.nmodels = 0; G.mvn = 0;
    G.mtop = RK_MAX_MODELS; G.mvt = MV_CAP;
}

/* transient slot: top of the id range downward, verts from the tail.
 * Persistent slots stay dense so they never see these; a
 * rk_models_drop_transient() makes them vanish with zero GPU work. */
int rk_model_mesh_add_top(const Vert *v, int n) {
    int slot = G.mtop - 1;
    if (slot < G.nmodels || n < 1 || G.mvt - n < G.mvn) return -1;
    G.mtop = slot; G.mvt -= n;
    G.modelOff[slot] = (VkDeviceSize)G.mvt;
    G.modelCount[slot] = (uint32_t)n;
    memcpy(G.mverts + G.mvt, v, (size_t)n * sizeof(Vert));
    return RK_MESH_BASE_MODEL + slot;
}

void rk_models_drop_transient(void) {
    G.mtop = RK_MAX_MODELS; G.mvt = MV_CAP;
}

int rk_model_mesh_add(const Vert *v, int n) {
    if (G.nmodels >= RK_MAX_MODELS || G.mvn + n > (int)(sizeof G.mverts / sizeof(Vert)))
        return -1;
    int id = G.nmodels++;
    G.modelOff[id] = (VkDeviceSize)G.mvn;
    G.modelCount[id] = (uint32_t)n;
    memcpy(G.mverts + G.mvn, v, (size_t)n * sizeof(Vert));
    G.mvn += n;
    return RK_MESH_BASE_MODEL + id;
}

void rk_models_commit(void) {
    vkDeviceWaitIdle(G.dev);              /* fuse is rare; simplicity wins */
    if (G.mbuf) { vkDestroyBuffer(G.dev, G.mbuf, NULL); vkFreeMemory(G.dev, G.mbufMem, NULL); }
    G.mbuf = VK_NULL_HANDLE; G.mbufMem = VK_NULL_HANDLE;
    /* transient tail verts ride the tail of the same buffer: upload
     * covers [0, vhi) where vhi reaches the transient base */
    int vhi = G.mvt < MV_CAP ? MV_CAP : G.mvn;
    if (!vhi) return;
    VkDeviceSize bytes = (VkDeviceSize)vhi * sizeof(Vert);
    void *map; VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, G.mverts, bytes);
    G.mbuf = buf_create(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.mbufMem, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkBufferCopy cp = {0, 0, bytes};
    vkCmdCopyBuffer(c, stag, G.mbuf, 1, &cp);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
}

/* ---------------- shaders ---------------- */

static VkShaderModule load_shader(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", SHADER_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing shader %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(n);
    if (fread(code, 1, n, f) != (size_t)n) { fclose(f); exit(1); }
    fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = (size_t)n; ci.pCode = code;
    VkShaderModule m;
    VKC(vkCreateShaderModule(G.dev, &ci, NULL, &m));
    free(code);
    return m;
}

/* ---------------- pipelines ---------------- */

typedef struct {
    VkShaderModule vs, fs;
    VkPrimitiveTopology topo;
    int instanced;                 /* binding1 = RkInst */
    VkVertexInputAttributeDescription attrs[8];
    VkVertexInputBindingDescription binds[2];
    uint32_t attrCount, bindCount;
    int blend, blendAdd, depthTest, depthWrite;
    VkCompareOp cmp;
    VkCullModeFlags cull;
    int depthBias, noDepth;
    VkSampleCountFlagBits samples;
    VkFormat fmt;                  /* 0 = surfFmt */
    VkPipelineLayout layout;       /* 0 = G.pipeLayout */
} PipeDesc;

static VkPipeline make_pipeline(const PipeDesc *d) {
    VkPipelineShaderStageCreateInfo st[2] = {0};
    uint32_t sn = 0;
    st[sn].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[sn].stage = VK_SHADER_STAGE_VERTEX_BIT; st[sn].module = d->vs; st[sn].pName = "main"; sn++;
    if (d->fs) {
        st[sn].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[sn].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[sn].module = d->fs; st[sn].pName = "main"; sn++;
    }

    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = d->bindCount; vin.pVertexBindingDescriptions = d->binds;
    vin.vertexAttributeDescriptionCount = d->attrCount; vin.pVertexAttributeDescriptions = d->attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d->topo;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (d->depthBias) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.5f;
        rs.depthBiasSlopeFactor = 2.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d->samples;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d->depthTest; ds.depthWriteEnable = d->depthWrite;
    ds.depthCompareOp = d->cmp;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState att = {0};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    if (d->blendAdd) {
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d->blend) {
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d->fs ? 1 : 0; cb.pAttachments = &att;

    VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;

    VkFormat colFmt = d->fmt ? d->fmt : G.surfFmt;
    VkPipelineRenderingCreateInfo rd = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (d->fs) {
        rd.colorAttachmentCount = 1; rd.pColorAttachmentFormats = &colFmt;
        rd.depthAttachmentFormat = d->noDepth ? VK_FORMAT_UNDEFINED
                                              : VK_FORMAT_D32_SFLOAT;
    } else {
        rd.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    }

    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rd;
    pi.stageCount = sn; pi.pStages = st;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dyn;
    pi.layout = d->layout ? d->layout : G.pipeLayout;
    VkPipeline p;
    VKC(vkCreateGraphicsPipelines(G.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p));
    return p;
}

static void fill_mesh_vertex_input(PipeDesc *d) {
    d->binds[0] = (VkVertexInputBindingDescription){0, sizeof(Vert), VK_VERTEX_INPUT_RATE_VERTEX};
    d->attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    d->attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
    d->attrs[2] = (VkVertexInputAttributeDescription){6, 0, VK_FORMAT_R32G32B32_SFLOAT, 24};
    d->attrs[3] = (VkVertexInputAttributeDescription){7, 0, VK_FORMAT_R32_SFLOAT, 36};
    d->attrCount = 4; d->bindCount = 1;
}

static void fill_inst_vertex_input(PipeDesc *d) {
    fill_mesh_vertex_input(d);
    d->binds[1] = (VkVertexInputBindingDescription){1, sizeof(RkInst), VK_VERTEX_INPUT_RATE_INSTANCE};
    d->attrs[4] = (VkVertexInputAttributeDescription){2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RkInst,pos)};
    d->attrs[5] = (VkVertexInputAttributeDescription){3, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RkInst,scale)};
    d->attrs[6] = (VkVertexInputAttributeDescription){4, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(RkInst,color)};
    d->attrs[7] = (VkVertexInputAttributeDescription){5, 1, VK_FORMAT_R32_UINT, offsetof(RkInst,flags)};
    d->attrCount = 8; d->bindCount = 2;
}

static void make_pipelines(void) {
    VkShaderModule wv = load_shader("world.vert");
    VkShaderModule wi = load_shader("world_i.vert");
    VkShaderModule sv = load_shader("shadow.vert");
    VkShaderModule wf = load_shader("world.frag");
    VkShaderModule gf = load_shader("grid.frag");
    VkShaderModule uv = load_shader("ui.vert");
    VkShaderModule uf = load_shader("ui.frag");
    VkShaderModule kv = load_shader("sky.vert");
    VkShaderModule kf = load_shader("sky.frag");

    PipeDesc d = {0};

    /* all pre-LDR geometry pipelines render into the HDR scene target */
    d.fmt = G.hdrFmt;

    /* grid: non-instanced, no cull, opaque */
    d.vs=wv; d.fs=gf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fill_mesh_vertex_input(&d);
    d.cull=VK_CULL_MODE_NONE; d.cmp=VK_COMPARE_OP_GREATER;
    d.depthTest=1; d.depthWrite=1; d.samples=G.samples;
    G.pGrid = make_pipeline(&d);

    /* icons / non-instanced lit */
    d.fs=wf; d.cull=VK_CULL_MODE_NONE;
    G.pWorld = make_pipeline(&d);

    /* instanced world blocks + overlays (no cull: closed shapes, immune to
       winding slips on generated meshes) */
    d.vs=wi; fill_inst_vertex_input(&d); d.cull=VK_CULL_MODE_NONE;
    G.pWorldInst = make_pipeline(&d);

    /* instanced ghost (blend) */
    d.blend=1; d.depthWrite=0;
    G.pGhostInst = make_pipeline(&d);

    /* instanced line boxes */
    d.topo=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; d.blend=1; d.depthWrite=0; d.cull=VK_CULL_MODE_NONE;
    G.pLinesInst = make_pipeline(&d);

    /* non-instanced dynamic lines (sculpt wireframe) */
    d.vs=wv; fill_mesh_vertex_input(&d);
    G.pMeshLine = make_pipeline(&d);

    /* shadow: depth-only, instanced, front-cull + depth bias, 1x samples */
    memset(&d, 0, sizeof d);
    d.vs=sv; d.fs=VK_NULL_HANDLE; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fill_inst_vertex_input(&d);
    d.cull=VK_CULL_MODE_FRONT_BIT;
    d.cmp=VK_COMPARE_OP_LESS; d.depthTest=1; d.depthWrite=1;
    d.depthBias=1; d.samples=VK_SAMPLE_COUNT_1_BIT;
    G.pShadow = make_pipeline(&d);

    /* UI: NDC quads, blend, depth ALWAYS + write (resets to far) */
    memset(&d, 0, sizeof d);
    d.vs=uv; d.fs=uf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.binds[0] = (VkVertexInputBindingDescription){0, sizeof(UIVert), VK_VERTEX_INPUT_RATE_VERTEX};
    d.attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    d.attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
    d.attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
    d.attrs[3] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32_SFLOAT, 32};
    d.attrCount=4; d.bindCount=1;
    d.blend=1; d.depthTest=1; d.depthWrite=1; d.cmp=VK_COMPARE_OP_ALWAYS;
    d.cull=VK_CULL_MODE_NONE; d.samples=G.samples;
    G.pUI = make_pipeline(&d);

    /* sky: fullscreen triangle, no vertex input, no depth touch */
    memset(&d, 0, sizeof d);
    d.vs=kv; d.fs=kf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.cull=VK_CULL_MODE_NONE; d.samples=G.samples; d.fmt=G.hdrFmt;
    G.pSky = make_pipeline(&d);

    /* post chain: fullscreen tri into hdrFmt (bloom) / surfFmt (composite) */
    VkShaderModule pv  = load_shader("post.vert");
    VkShaderModule dnf = load_shader("down.frag");
    VkShaderModule upf = load_shader("up.frag");
    VkShaderModule psf = load_shader("post.frag");
    memset(&d, 0, sizeof d);
    d.vs=pv; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.cull=VK_CULL_MODE_NONE; d.samples=VK_SAMPLE_COUNT_1_BIT;
    d.noDepth=1; d.fmt=G.hdrFmt; d.layout=G.postLayout;
    d.fs=dnf;               G.pDown = make_pipeline(&d);
    d.fs=upf; d.blendAdd=1; G.pUp   = make_pipeline(&d);
    d.fs=psf; d.blendAdd=0; d.fmt=0; G.pPost = make_pipeline(&d);

    /* LDR pass: 1x variants of icon-shaded world + UI for the swapchain */
    memset(&d, 0, sizeof d);
    d.vs=wv; d.fs=wf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fill_mesh_vertex_input(&d);
    d.cull=VK_CULL_MODE_NONE; d.cmp=VK_COMPARE_OP_GREATER;
    d.depthTest=1; d.depthWrite=1; d.samples=VK_SAMPLE_COUNT_1_BIT;
    G.pIcon = make_pipeline(&d);
    memset(&d, 0, sizeof d);
    d.vs=uv; d.fs=uf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.binds[0] = (VkVertexInputBindingDescription){0, sizeof(UIVert), VK_VERTEX_INPUT_RATE_VERTEX};
    d.attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    d.attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
    d.attrs[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
    d.attrs[3] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32_SFLOAT, 32};
    d.attrCount=4; d.bindCount=1;
    d.blend=1; d.depthTest=1; d.depthWrite=1; d.cmp=VK_COMPARE_OP_ALWAYS;
    d.cull=VK_CULL_MODE_NONE; d.samples=VK_SAMPLE_COUNT_1_BIT;
    G.pUIFlat = make_pipeline(&d);

    vkDestroyShaderModule(G.dev, wv, NULL); vkDestroyShaderModule(G.dev, wi, NULL);
    vkDestroyShaderModule(G.dev, sv, NULL); vkDestroyShaderModule(G.dev, wf, NULL);
    vkDestroyShaderModule(G.dev, gf, NULL); vkDestroyShaderModule(G.dev, uv, NULL);
    vkDestroyShaderModule(G.dev, uf, NULL);
    vkDestroyShaderModule(G.dev, kv, NULL); vkDestroyShaderModule(G.dev, kf, NULL);
    vkDestroyShaderModule(G.dev, pv, NULL); vkDestroyShaderModule(G.dev, dnf, NULL);
    vkDestroyShaderModule(G.dev, upf, NULL); vkDestroyShaderModule(G.dev, psf, NULL);
}

/* ---------------- hot reload (F8): glslc sources -> spv, rebuild pipelines --- */

static const char *SHADER_NAMES[] = {
    "world.vert","world_i.vert","world.frag","grid.frag","shadow.vert",
    "ui.vert","ui.frag","sky.vert","sky.frag",
    "post.vert","down.frag","up.frag","post.frag"
};

/* run glslc without a console flash on the WIN32 app */
static int run_glslc(const char *src, const char *dst) {
#if defined(_WIN32) && defined(GLSLC)
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" -o \"%s\"", GLSLC, src, dst);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    /* child's stderr/stdout -> our (possibly piped) stderr -> varlera.log */
    HANDLE he = (HANDLE)_get_osfhandle(_fileno(stderr));
    si.hStdOutput = he; si.hStdError = he;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) return 1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return rc != 0;
#elif defined(GLSLC)
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" -o \"%s\"", GLSLC, src, dst);
    return system(cmd) != 0;
#else
    (void)src; (void)dst; return 1;
#endif
}

/* recompile all shaders from source dir into SHADER_DIR, then rebuild every
   pipeline in place. Returns 1 when all compiles succeeded. Failed compiles
   leave the previous .spv in place (glslc writes only on success). */
int rk_reload_shaders(void) {
#if defined(GLSLC) && defined(SHADER_SRC)
    int fail = 0;
    for (int i = 0; i < (int)(sizeof SHADER_NAMES / sizeof SHADER_NAMES[0]); i++) {
        char src[1024], dst[1024];
        snprintf(src, sizeof src, "%s/%s", SHADER_SRC, SHADER_NAMES[i]);
        snprintf(dst, sizeof dst, "%s/%s.spv", SHADER_DIR, SHADER_NAMES[i]);
        if (run_glslc(src, dst)) { fail++; fprintf(stderr, "glslc failed: %s\n", src); }
    }
    vkDeviceWaitIdle(G.dev);
    VkPipeline *pp[] = {&G.pGrid, &G.pWorld, &G.pWorldInst, &G.pGhostInst,
                        &G.pLinesInst, &G.pMeshLine, &G.pShadow, &G.pUI, &G.pSky,
                        &G.pDown, &G.pUp, &G.pPost, &G.pIcon, &G.pUIFlat};
    for (int i = 0; i < (int)(sizeof pp / sizeof pp[0]); i++)
        vkDestroyPipeline(G.dev, *pp[i], NULL);
    make_pipelines();
    fprintf(stderr, "shader reload: %s\n", fail ? "COMPILE ERRORS" : "ok");
    return !fail;
#else
    fprintf(stderr, "shader reload unavailable (GLSLC/SHADER_SRC not defined)\n");
    return 0;
#endif
}

/* ---------------- swapchain ---------------- */

static VkSurfaceFormatKHR pick_surface_format(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(G.phys, G.surf, &n, NULL);
    VkSurfaceFormatKHR *f = malloc(n * sizeof *f);
    vkGetPhysicalDeviceSurfaceFormatsKHR(G.phys, G.surf, &n, f);
    VkSurfaceFormatKHR pick = f[0];
    for (uint32_t i = 0; i < n; i++)
        if (f[i].format == VK_FORMAT_B8G8R8A8_SRGB) { pick = f[i]; break; }
    free(f);
    return pick;
}

static VkPresentModeKHR pick_present_mode(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(G.phys, G.surf, &n, NULL);
    VkPresentModeKHR *m = malloc(n * sizeof *m);
    vkGetPhysicalDeviceSurfacePresentModesKHR(G.phys, G.surf, &n, m);
    VkPresentModeKHR pick = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++)
        if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) { pick = m[i]; break; }
    free(m);
    return pick;
}

static void make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkSampleCountFlagBits samples,
                       uint32_t mips,
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = (VkExtent3D){w, h, 1}; ii.mipLevels = mips; ii.arrayLayers = 1;
    ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(G.dev, &ii, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(G.dev, *img, &req);
    *mem = mem_alloc(req.size, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkBindImageMemory(G.dev, *img, *mem, 0));
    if (!view) return;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, mips, 0, 1};
    VKC(vkCreateImageView(G.dev, &vi, NULL, view));
}

/* single-mip view of a mipped image (bloom chain attach/sample views) */
static VkImageView mip_view(VkImage img, VkFormat fmt, uint32_t mip) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
    VkImageView v;
    VKC(vkCreateImageView(G.dev, &vi, NULL, &v));
    return v;
}

/* post-chain targets sized to the framebuffer: HDR scene image (msaa
   resolve dst / direct target), bloom mip chain, 1x depth for the LDR
   UI pass (only needed when the main depth buffer is multisampled). */
static void post_create(void) {
    VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
    make_image(G.ext.width, G.ext.height, G.hdrFmt, u,
               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
               &G.hdrImg, &G.hdrMem, &G.hdrView);
    if (G.bloomOk) {
        uint32_t bw = G.ext.width / 2 ? G.ext.width / 2 : 1;
        uint32_t bh = G.ext.height / 2 ? G.ext.height / 2 : 1;
        make_image(bw, bh, G.hdrFmt, u, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_SAMPLE_COUNT_1_BIT, BLOOM_M,
                   &G.bloomImg, &G.bloomMem, NULL);
        for (int i = 0; i < BLOOM_M; i++)
            G.bloomMipView[i] = mip_view(G.bloomImg, G.hdrFmt, (uint32_t)i);
    }
    if (G.samples != VK_SAMPLE_COUNT_1_BIT)
        make_image(G.ext.width, G.ext.height, VK_FORMAT_D32_SFLOAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
                   &G.uiDepthImg, &G.uiDepthMem, &G.uiDepthView);
}

static void post_destroy(void) {
    if (G.hdrView) vkDestroyImageView(G.dev, G.hdrView, NULL);
    if (G.hdrImg) { vkDestroyImage(G.dev, G.hdrImg, NULL); vkFreeMemory(G.dev, G.hdrMem, NULL); }
    for (int i = 0; i < BLOOM_M; i++)
        if (G.bloomMipView[i]) vkDestroyImageView(G.dev, G.bloomMipView[i], NULL);
    if (G.bloomImg) { vkDestroyImage(G.dev, G.bloomImg, NULL); vkFreeMemory(G.dev, G.bloomMem, NULL); }
    if (G.uiDepthView) vkDestroyImageView(G.dev, G.uiDepthView, NULL);
    if (G.uiDepthImg) { vkDestroyImage(G.dev, G.uiDepthImg, NULL); vkFreeMemory(G.dev, G.uiDepthMem, NULL); }
    G.hdrView = G.uiDepthView = VK_NULL_HANDLE;
    G.hdrImg = G.bloomImg = G.uiDepthImg = VK_NULL_HANDLE;
    memset(G.bloomMipView, 0, sizeof G.bloomMipView);
    G.bloomLive = 0;
}

/* post descriptor sets reference per-swapchain views -> rewrite on resize */
static void post_ds_write(void) {
    if (!G.postDS[0]) return;
    for (int i = 0; i <= BLOOM_M; i++) {
        VkDescriptorImageInfo dii[2] = {
            {G.postSamp, (i == 0 || !G.bloomOk) ? G.hdrView : G.bloomMipView[i - 1],
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {G.postSamp, G.bloomOk ? G.bloomMipView[0] : G.hdrView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet w[2] = {0};
        for (int k = 0; k < 2; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = G.postDS[i]; w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &dii[k];
        }
        vkUpdateDescriptorSets(G.dev, 2, w, 0, NULL);
    }
}

static void swapchain_destroy(void) {
    for (uint32_t i = 0; i < G.imgCount; i++)
        vkDestroyImageView(G.dev, G.views[i], NULL);
    post_destroy();
    if (G.msaaView) vkDestroyImageView(G.dev, G.msaaView, NULL);
    if (G.depthView) vkDestroyImageView(G.dev, G.depthView, NULL);
    if (G.msaaImg) { vkDestroyImage(G.dev, G.msaaImg, NULL); vkFreeMemory(G.dev, G.msaaMem, NULL); }
    if (G.depthImg) { vkDestroyImage(G.dev, G.depthImg, NULL); vkFreeMemory(G.dev, G.depthMem, NULL); }
    G.msaaView = G.depthView = VK_NULL_HANDLE;
    G.msaaImg = G.depthImg = VK_NULL_HANDLE;
    if (G.swap) vkDestroySwapchainKHR(G.dev, G.swap, NULL);
    G.swap = VK_NULL_HANDLE;
}

static void swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.phys, G.surf, &caps));
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(G.win, &fw, &fh);
    VkExtent2D ext = { (uint32_t)fw, (uint32_t)fh };
    if (caps.currentExtent.width != UINT32_MAX) ext = caps.currentExtent;
    if (ext.width == 0 || ext.height == 0) { G.ext = ext; return; }
    ext.width  = (uint32_t)clampf((float)ext.width,  (float)caps.minImageExtent.width,  (float)caps.maxImageExtent.width);
    ext.height = (uint32_t)clampf((float)ext.height, (float)caps.minImageExtent.height, (float)caps.maxImageExtent.height);

    VkSurfaceFormatKHR sf = pick_surface_format();
    G.surfFmt = sf.format; G.surfCS = sf.colorSpace;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = G.surf; ci.minImageCount = count;
    ci.imageFormat = G.surfFmt; ci.imageColorSpace = G.surfCS;
    ci.imageExtent = ext; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   /* screenshot readback */
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pick_present_mode();
    ci.clipped = VK_TRUE;
    VKC(vkCreateSwapchainKHR(G.dev, &ci, NULL, &G.swap));

    vkGetSwapchainImagesKHR(G.dev, G.swap, &G.imgCount, NULL);
    if (G.imgCount > MAX_IMAGES) G.imgCount = MAX_IMAGES;
    vkGetSwapchainImagesKHR(G.dev, G.swap, &G.imgCount, G.imgs);
    for (uint32_t i = 0; i < G.imgCount; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = G.imgs[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = G.surfFmt;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKC(vkCreateImageView(G.dev, &vi, NULL, &G.views[i]));
    }
    if (G.samples != VK_SAMPLE_COUNT_1_BIT)
        make_image(ext.width, ext.height, G.hdrFmt,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, G.samples, 1,
                   &G.msaaImg, &G.msaaMem, &G.msaaView);
    make_image(ext.width, ext.height, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, G.samples, 1,
               &G.depthImg, &G.depthMem, &G.depthView);
    G.ext = ext;
    post_create();

    if (G.imgCount > G.renderDoneCount) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = G.renderDoneCount; i < G.imgCount; i++)
            VKC(vkCreateSemaphore(G.dev, &si, NULL, &G.renderDone[i]));
        G.renderDoneCount = G.imgCount;
    }
}

void rk_swapchain_recreate(void) {
    vkDeviceWaitIdle(G.dev);
    swapchain_destroy();
    swapchain_create();
    post_ds_write();
    G.needResize = 0;
}

/* ---------------- init ---------------- */

static void pick_physical_device(void) {
    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(G.inst, &n, NULL));
    if (!n) { fprintf(stderr, "no vulkan devices\n"); exit(1); }
    VkPhysicalDevice *ds = malloc(n * sizeof *ds);
    VKC(vkEnumeratePhysicalDevices(G.inst, &n, ds));
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(ds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &qn, NULL);
        VkQueueFamilyProperties *qp = malloc(qn * sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &qn, qp);
        int found = -1;
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(ds[i], q, G.surf, &pres);
            if (pres && (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { found = (int)q; break; }
        }
        free(qp);
        if (found < 0) continue;
        int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 10 : 1;
        if (score > bestScore) { bestScore = score; best = ds[i]; G.qfam = (uint32_t)found; }
    }
    free(ds);
    if (!best) { fprintf(stderr, "no suitable device\n"); exit(1); }
    G.phys = best;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(best, &p);
    fprintf(stderr, "GPU: %s\n", p.deviceName);
}

int rk_init(GLFWwindow *win) {
    memset(&G, 0, sizeof G);
    rk_models_reset();              /* mtop/mvt tail cursors */
    G.win = win;

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Varlera";
    app.apiVersion = VK_API_VERSION_1_3;

    uint32_t extCount = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&extCount);
    const char *extra[16];
    uint32_t total = extCount;
    for (uint32_t i = 0; i < extCount; i++) extra[i] = exts[i];
#ifndef NDEBUG
    extra[total++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = total; ii.ppEnabledExtensionNames = extra;
#ifndef NDEBUG
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layerN = 0, availN = 0;
    vkEnumerateInstanceLayerProperties(&availN, NULL);
    VkLayerProperties *avail = malloc(availN * sizeof *avail);
    vkEnumerateInstanceLayerProperties(&availN, avail);
    for (uint32_t i = 0; i < availN; i++)
        if (!strcmp(avail[i].layerName, layers[0])) layerN = 1;
    free(avail);
    ii.enabledLayerCount = layerN; ii.ppEnabledLayerNames = layers;
#endif
    VKC(vkCreateInstance(&ii, NULL, &G.inst));
    VKC(glfwCreateWindowSurface(G.inst, win, NULL, &G.surf));
    pick_physical_device();

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = G.qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char *devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f13;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = devExts;
    VKC(vkCreateDevice(G.phys, &di, NULL, &G.dev));
    vkGetDeviceQueue(G.dev, G.qfam, 0, &G.queue);

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(G.phys, &pp);
    VkSampleCountFlags sc = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts;
    G.samples = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    G.hasTS = pp.limits.timestampComputeAndGraphics == VK_TRUE;
    G.tsPeriod = pp.limits.timestampPeriod;

    /* HDR target: R16G16B16A16_SFLOAT where supported (universal on
       desktop); bloom needs attachment blending on the same format. */
    G.hdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(G.phys, G.hdrFmt, &fp);
    G.hdrOk = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
              (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    G.bloomOk = G.hdrOk &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    if (!G.hdrOk) G.hdrFmt = VK_FORMAT_B8G8R8A8_UNORM;

    swapchain_create();

    /* shadow map + compare sampler */
    G.shSize = SHADOW_SIZE;                 /* default; WSet can resize */
    make_image(G.shSize, G.shSize, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
               &G.shadowImg, &G.shadowMem, &G.shadowView);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VKC(vkCreateSampler(G.dev, &sci, NULL, &G.shadowSamp));

    /* texture atlas image + pixel-art sampler (nearest, clamp) */
    make_image(RK_TEX_ATLAS, RK_TEX_ATLAS, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
               &G.texImg, &G.texMem, &G.texView);
    VkSamplerCreateInfo tsi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    tsi.magFilter = tsi.minFilter = VK_FILTER_NEAREST;
    tsi.addressModeU = tsi.addressModeV = tsi.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(G.dev, &tsi, NULL, &G.texSamp));

    /* post-chain sampler: linear + clamp (bloom down/up, composite) */
    VkSamplerCreateInfo psi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    psi.magFilter = psi.minFilter = VK_FILTER_LINEAR;
    psi.addressModeU = psi.addressModeV = psi.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(G.dev, &psi, NULL, &G.postSamp));

    /* descriptors: set0 = ubo/frame, set1 = shadow sampler + tex atlas */
    VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1; li.pBindings = &b0;
    VKC(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.setLayout0));
    VkDescriptorSetLayoutBinding b1[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    li.bindingCount = 2; li.pBindings = b1;
    VKC(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.setLayout1));
    /* post chain: b0 = source/hdr, b1 = bloom mip0 (composite) */
    VkDescriptorSetLayoutBinding b2[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    li.bindingCount = 2; li.pBindings = b2;
    VKC(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.postSetLayout));

    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 + 2 * (BLOOM_M + 1)}
    };
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = MAX_FRAMES + 1 + BLOOM_M + 1;
    pci.poolSizeCount = 2; pci.pPoolSizes = psz;
    VKC(vkCreateDescriptorPool(G.dev, &pci, NULL, &G.pool));

    {
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = G.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &G.setLayout1;
        VKC(vkAllocateDescriptorSets(G.dev, &dai, &G.set1));
        VkDescriptorImageInfo dii[2] = {
            {G.shadowSamp, G.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {G.texSamp, G.texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet w[2];
        memset(w, 0, sizeof w);
        for (int i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = G.set1; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &dii[i];
        }
        vkUpdateDescriptorSets(G.dev, 2, w, 0, NULL);
    }
    for (int i = 0; i <= BLOOM_M; i++) {
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = G.pool; dai.descriptorSetCount = 1;
        dai.pSetLayouts = &G.postSetLayout;
        VKC(vkAllocateDescriptorSets(G.dev, &dai, &G.postDS[i]));
    }
    post_ds_write();

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkDescriptorSetLayout lset[2] = {G.setLayout0, G.setLayout1};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = lset;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(G.dev, &pli, NULL, &G.pipeLayout));
    VkPushConstantRange ppcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(PostPC)};
    pli.setLayoutCount = 1; pli.pSetLayouts = &G.postSetLayout;
    pli.pPushConstantRanges = &ppcr;
    VKC(vkCreatePipelineLayout(G.dev, &pli, NULL, &G.postLayout));

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = G.qfam;
    VKC(vkCreateCommandPool(G.dev, &cpi, NULL, &G.cmdPool));

    for (int i = 0; i < MAX_FRAMES; i++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(G.dev, &cai, &G.fr[i].cmd));
        VkFenceCreateInfo fi2 = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi2.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKC(vkCreateFence(G.dev, &fi2, NULL, &G.fr[i].fence));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(G.dev, &si, NULL, &G.fr[i].acq));
        G.fr[i].ubo = buf_create(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HOST_MEM,
            &G.fr[i].uboMem, &G.fr[i].uboMap);
        G.fr[i].inst = buf_create(MAX_INST * sizeof(RkInst), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].instMem, &G.fr[i].instMap);
        G.fr[i].uib = buf_create(MAX_UI_QUADS * 6 * sizeof(UIVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].uibMem, &G.fr[i].uiMap);
        G.fr[i].mesh = buf_create(MAX_DYN_VERTS * sizeof(Vert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].meshMem, &G.fr[i].meshMap);
        if (G.hasTS) {
            VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 4;
            VKC(vkCreateQueryPool(G.dev, &qpi, NULL, &G.fr[i].qp));
        }
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = G.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &G.setLayout0;
        VKC(vkAllocateDescriptorSets(G.dev, &dai, &G.fr[i].set0));
        VkDescriptorBufferInfo dbi = {G.fr[i].ubo, 0, sizeof(UBO)};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = G.fr[i].set0; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(G.dev, 1, &w, 0, NULL);
    }

    build_mesh_data();
    make_pipelines();

    /* icon view-projection (reversed-Z ortho to match GREATER compare) */
    {
        m4 v, p;
        v3 eye = {2.2f, 1.9f, 2.2f}, at = {0,0,0}, up = {0,1,0};
        m4_look_at(v, eye, at, up);
        m4_ortho_rz(p, -0.95f, 0.95f, -0.95f, 0.95f, 0.1f, 10.0f);
        m4_mul(G.ubo.iconVP, p, v);
    }
    m4_identity(G.ubo.viewProj);
    m4_identity(G.ubo.shadowVP);
    G.ubo.camPos[0]=0; G.ubo.camPos[1]=10; G.ubo.camPos[2]=10;
    { v3 l = rk_light_dir(); memcpy(G.ubo.lightDir, l.e, sizeof l); }
    G.ubo.fogColor[0]=0.56f; G.ubo.fogColor[1]=0.66f; G.ubo.fogColor[2]=0.82f; G.ubo.fogColor[3]=1;
    G.ubo.fogParams[0]=60.0f; G.ubo.fogParams[1]=170.0f;
    G.ubo.fogParams[2]=1.0f;  G.ubo.fogParams[3]=0.5f;   /* curve, PCF px */
    G.ubo.groundCol[0]=0.58f; G.ubo.groundCol[1]=0.60f;
    G.ubo.groundCol[2]=0.64f; G.ubo.groundCol[3]=0;   /* w = ground style */
    G.bloomAmt = 0.55f;
    G.lightDir = v3_norm(v3_make(0.72f, 0.55f, 0.30f));
    G.dayK = 1.0f;
    G.envSky[0] = 0.56f; G.envSky[1] = 0.66f; G.envSky[2] = 0.82f;
    G.envGround[0] = 0.58f; G.envGround[1] = 0.60f; G.envGround[2] = 0.64f;
    rk_set_day(1.0f);
    return 1;
}

/* runtime shadow-map resize: recreate the depth image + rewrite the set1
 * sampler binding. Called from the main loop only when the size changed. */
void rk_shadow_resize(int size) {
    if (!G.dev || size <= 0 || size == G.shSize) return;
    vkDeviceWaitIdle(G.dev);
    vkDestroyImageView(G.dev, G.shadowView, NULL);
    vkDestroyImage(G.dev, G.shadowImg, NULL);
    vkFreeMemory(G.dev, G.shadowMem, NULL);
    make_image((uint32_t)size, (uint32_t)size, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
               &G.shadowImg, &G.shadowMem, &G.shadowView);
    G.shSize = size;
    VkDescriptorImageInfo dii = {G.shadowSamp, G.shadowView,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = G.set1; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(G.dev, 1, &w, 0, NULL);
}

void rk_set_env(const float sky[3], const float ground[3], int gstyle) {
    if (sky) memcpy(G.envSky, sky, 12);
    if (ground) memcpy(G.envGround, ground, 12);
    G.envStyle = gstyle;
    rk_set_day(G.dayK);                 /* reapply current daylight factor */
}

/* day/night: scale env colors toward a dim blue night tint (k=1 daylight) */
void rk_set_day(float k) {
    static const float NIGHT[3] = {0.10f, 0.14f, 0.32f};
    G.dayK = k;
    for (int i = 0; i < 3; i++) {
        float m = NIGHT[i] + (1.0f - NIGHT[i]) * k;
        G.ubo.fogColor[i]  = G.envSky[i] * m;
        G.ubo.groundCol[i] = G.envGround[i] * m;
    }
    G.ubo.groundCol[3] = (float)G.envStyle;
}

void rk_set_fog(float near_, float far_, float curve, float pcf) {
    G.ubo.fogParams[0] = near_; G.ubo.fogParams[1] = far_;
    G.ubo.fogParams[2] = curve; G.ubo.fogParams[3] = pcf;
}
void rk_set_bloom(float amt) { G.bloomAmt = amt; }

void rk_shutdown(void) {
    vkDeviceWaitIdle(G.dev);
    for (int i = 0; i < MAX_FRAMES; i++) {
        vkUnmapMemory(G.dev, G.fr[i].uboMem);
        vkUnmapMemory(G.dev, G.fr[i].instMem);
        vkUnmapMemory(G.dev, G.fr[i].uibMem);
        vkDestroyBuffer(G.dev, G.fr[i].ubo, NULL);
        vkDestroyBuffer(G.dev, G.fr[i].inst, NULL);
        vkDestroyBuffer(G.dev, G.fr[i].uib, NULL);
        vkFreeMemory(G.dev, G.fr[i].uboMem, NULL);
        vkFreeMemory(G.dev, G.fr[i].instMem, NULL);
        vkFreeMemory(G.dev, G.fr[i].uibMem, NULL);
        vkUnmapMemory(G.dev, G.fr[i].meshMem);
        vkDestroyBuffer(G.dev, G.fr[i].mesh, NULL);
        vkFreeMemory(G.dev, G.fr[i].meshMem, NULL);
        vkDestroyFence(G.dev, G.fr[i].fence, NULL);
        vkDestroySemaphore(G.dev, G.fr[i].acq, NULL);
        if (G.fr[i].qp) vkDestroyQueryPool(G.dev, G.fr[i].qp, NULL);
    }
    for (uint32_t i = 0; i < G.renderDoneCount; i++)
        vkDestroySemaphore(G.dev, G.renderDone[i], NULL);
    vkDestroyBuffer(G.dev, G.vbuf, NULL);
    vkFreeMemory(G.dev, G.vbufMem, NULL);
    if (G.mbuf) {
        vkDestroyBuffer(G.dev, G.mbuf, NULL);
        vkFreeMemory(G.dev, G.mbufMem, NULL);
    }
    if (G.shotBuf) {
        vkUnmapMemory(G.dev, G.shotMem);
        vkDestroyBuffer(G.dev, G.shotBuf, NULL);
        vkFreeMemory(G.dev, G.shotMem, NULL);
    }
    vkDestroyImageView(G.dev, G.shadowView, NULL);
    vkDestroyImage(G.dev, G.shadowImg, NULL);
    vkFreeMemory(G.dev, G.shadowMem, NULL);
    vkDestroySampler(G.dev, G.shadowSamp, NULL);
    vkDestroyImageView(G.dev, G.texView, NULL);
    vkDestroyImage(G.dev, G.texImg, NULL);
    vkFreeMemory(G.dev, G.texMem, NULL);
    vkDestroySampler(G.dev, G.texSamp, NULL);
    vkDestroyPipeline(G.dev, G.pGrid, NULL);
    vkDestroyPipeline(G.dev, G.pWorld, NULL);
    vkDestroyPipeline(G.dev, G.pWorldInst, NULL);
    vkDestroyPipeline(G.dev, G.pGhostInst, NULL);
    vkDestroyPipeline(G.dev, G.pLinesInst, NULL);
    vkDestroyPipeline(G.dev, G.pMeshLine, NULL);
    vkDestroyPipeline(G.dev, G.pShadow, NULL);
    vkDestroyPipeline(G.dev, G.pUI, NULL);
    vkDestroyPipeline(G.dev, G.pSky, NULL);
    vkDestroyPipeline(G.dev, G.pDown, NULL);
    vkDestroyPipeline(G.dev, G.pUp, NULL);
    vkDestroyPipeline(G.dev, G.pPost, NULL);
    vkDestroyPipeline(G.dev, G.pIcon, NULL);
    vkDestroyPipeline(G.dev, G.pUIFlat, NULL);
    vkDestroySampler(G.dev, G.postSamp, NULL);
    vkDestroyPipelineLayout(G.dev, G.postLayout, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.postSetLayout, NULL);
    vkDestroyPipelineLayout(G.dev, G.pipeLayout, NULL);
    vkDestroyDescriptorPool(G.dev, G.pool, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.setLayout0, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.setLayout1, NULL);
    vkDestroyCommandPool(G.dev, G.cmdPool, NULL);
    swapchain_destroy();
    vkDestroyDevice(G.dev, NULL);
    vkDestroySurfaceKHR(G.inst, G.surf, NULL);
    vkDestroyInstance(G.inst, NULL);
}