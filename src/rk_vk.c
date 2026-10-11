/* rk_vk.c - Vulkan device/swapchain/pipeline init, image+buffer
 * resources, texture atlas + model-mesh uploads. Frame path lives
 * in rk.c; shared state is in rk_int.h. */
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


static VkBuffer buf_create(VkDeviceSize size, VkBufferUsageFlags usage,
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

void rk_models_reset(void) { G.nmodels = 0; G.mvn = 0; }

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
    if (!G.mvn) return;
    VkDeviceSize bytes = (VkDeviceSize)G.mvn * sizeof(Vert);
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
    int blend, depthTest, depthWrite;
    VkCompareOp cmp;
    VkCullModeFlags cull;
    int depthBias;
    VkSampleCountFlagBits samples;
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
    if (d->blend) {
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

    VkPipelineRenderingCreateInfo rd = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (d->fs) {
        rd.colorAttachmentCount = 1; rd.pColorAttachmentFormats = &G.surfFmt;
        rd.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
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
    pi.layout = G.pipeLayout;
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

    PipeDesc d = {0};

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

    vkDestroyShaderModule(G.dev, wv, NULL); vkDestroyShaderModule(G.dev, wi, NULL);
    vkDestroyShaderModule(G.dev, sv, NULL); vkDestroyShaderModule(G.dev, wf, NULL);
    vkDestroyShaderModule(G.dev, gf, NULL); vkDestroyShaderModule(G.dev, uv, NULL);
    vkDestroyShaderModule(G.dev, uf, NULL);
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
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = (VkExtent3D){w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(G.dev, &ii, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(G.dev, *img, &req);
    *mem = mem_alloc(req.size, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkBindImageMemory(G.dev, *img, *mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VKC(vkCreateImageView(G.dev, &vi, NULL, view));
}

static void swapchain_destroy(void) {
    for (uint32_t i = 0; i < G.imgCount; i++)
        vkDestroyImageView(G.dev, G.views[i], NULL);
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
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
        make_image(ext.width, ext.height, G.surfFmt,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, G.samples, &G.msaaImg, &G.msaaMem, &G.msaaView);
    make_image(ext.width, ext.height, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, G.samples, &G.depthImg, &G.depthMem, &G.depthView);
    G.ext = ext;

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

    swapchain_create();

    /* shadow map + compare sampler */
    make_image(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT,
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
               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
               &G.texImg, &G.texMem, &G.texView);
    VkSamplerCreateInfo tsi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    tsi.magFilter = tsi.minFilter = VK_FILTER_NEAREST;
    tsi.addressModeU = tsi.addressModeV = tsi.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(G.dev, &tsi, NULL, &G.texSamp));

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

    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}
    };
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = MAX_FRAMES + 1; pci.poolSizeCount = 2; pci.pPoolSizes = psz;
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

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkDescriptorSetLayout lset[2] = {G.setLayout0, G.setLayout1};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = lset;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(G.dev, &pli, NULL, &G.pipeLayout));

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
    G.ubo.groundCol[0]=0.58f; G.ubo.groundCol[1]=0.60f;
    G.ubo.groundCol[2]=0.64f; G.ubo.groundCol[3]=1;
    return 1;
}

void rk_set_env(const float sky[3], const float ground[3]) {
    if (sky) memcpy(G.ubo.fogColor, sky, 12);
    if (ground) memcpy(G.ubo.groundCol, ground, 12);
}

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
    }
    for (uint32_t i = 0; i < G.renderDoneCount; i++)
        vkDestroySemaphore(G.dev, G.renderDone[i], NULL);
    vkDestroyBuffer(G.dev, G.vbuf, NULL);
    vkFreeMemory(G.dev, G.vbufMem, NULL);
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