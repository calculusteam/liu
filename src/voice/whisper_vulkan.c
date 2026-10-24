/*
 * Liu - Vulkan compute backend for Whisper.
 *
 * Structure follows whisper_metal.m: weights split into a quantized buffer
 * dequantized inside the kernels and an f32 buffer for the small tensors, one
 * command buffer for the whole encoder, and the same tile shapes. What is
 * different is forced by the API rather than chosen:
 *
 *   - Everything is loaded at runtime. VK_NO_PROTOTYPES plus dlopen means the
 *     binary has no libvulkan dependency and simply falls back when there is
 *     no driver.
 *   - Vulkan has no implicit ordering between dispatches, so every one that
 *     reads what the previous wrote is separated by an explicit barrier.
 *     Metal's compute encoder inserts those itself; forgetting one here is a
 *     race that shows up as an occasionally wrong transcript.
 *   - Descriptor sets are consumed, not rebound. One set per dispatch out of
 *     a pre-allocated pool, because a set may not be rewritten while a
 *     submitted command buffer still refers to it.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice/whisper_vulkan.h"
#include "voice/whisper_scratch.h"
#include "voice/whisper_mel.h"
#include "voice/whisper_metal_params.h"
#include "voice/whisper_quant.h"
#include "voice/whisper_vulkan_spv.h"

/* Attention tile, mirroring shaders/attention.comp. Separate from the Metal
 * constants because the two kernels are tiled differently — no cooperative
 * matrices here — and a shared name would imply they must match. */
#define WV_ATTN_QB 16

/* GEMM tile, mirroring shaders/gemm.comp. */
#define WV_BM 64
#define WV_BN 64

/* Descriptor sets are handed out one per dispatch and reset per command
 * buffer. The encoder is 32 layers x 11 dispatches plus the stem and the
 * cross-attention pass; 1024 leaves room without being a real allocation. */
#define WV_MAX_SETS 1024
#define WV_BINDINGS 4

/* Staging chunk for the weight upload when the device has no host-visible
 * device-local heap. 64 MB is large enough that a 573 MB model is a handful
 * of copies and small enough to allocate on a modest GPU. */
#define WV_STAGE_BYTES (64u << 20)

typedef enum {
    WV_PIPE_GEMM = 0,
    WV_PIPE_MATVEC,
    WV_PIPE_CONV1D,
    WV_PIPE_LAYERNORM,
    WV_PIPE_ATTENTION,
    WV_PIPE_ELEMENTWISE,
    WV_PIPE_TRANSPOSE_POS,
    WV_PIPE_EMBED,
    WV_PIPE_KVSTORE,
    WV_PIPE_COUNT,
} WvPipe;

/* Push-constant block for the elementwise shader: the Metal side has separate
 * add and gelu kernels, this one has a mode switch. */
struct WvElem { i32 n, mode, pad0, pad1; };

typedef struct { u64 off; i32 dtype; i32 K, N; } WvMat;   /* into wbuf */
typedef struct { u64 off; i32 n; }               WvVec;   /* into sbuf, float index */

typedef struct {
    WvVec attn_ln_w, attn_ln_b;
    WvMat attn_q_w;  WvVec attn_q_b;
    WvMat attn_k_w;
    WvMat attn_v_w;  WvVec attn_v_b;
    WvMat attn_o_w;  WvVec attn_o_b;

    WvVec cross_ln_w, cross_ln_b;
    WvMat cross_q_w; WvVec cross_q_b;
    WvMat cross_k_w;
    WvMat cross_v_w; WvVec cross_v_b;
    WvMat cross_o_w; WvVec cross_o_b;

    WvVec mlp_ln_w, mlp_ln_b;
    WvMat mlp_0_w;   WvVec mlp_0_b;
    WvMat mlp_2_w;   WvVec mlp_2_b;
} WvLayer;

typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; void *mapped; } WvBuf;

/* ---- loader ------------------------------------------------------------- */

typedef struct {
    void *lib;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetDeviceQueue GetDeviceQueue;

    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;

    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;

    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdFillBuffer CmdFillBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} WvApi;

struct WhisperVulkan {
    WvApi api;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    u32 queue_family;
    char device_name[256];

    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkPipeline pipes[WV_PIPE_COUNT];
    VkShaderModule modules[WV_PIPE_COUNT];
    VkDescriptorPool desc_pool;
    VkDescriptorSet sets[WV_MAX_SETS];
    i32 set_next;

    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;

    WvBuf wbuf, sbuf;
    WvBuf mel, ff, x, xn, q, k, v, att;
    WvBuf cross_k, cross_v, self_k, self_v;
    WvBuf ids, logits;
    WvBuf host;          /* host-visible staging for uploads and readback */

    i32 n_state, n_ctx_a, n_ctx_t, n_mels, n_head_a, n_head_t, hd_a, hd_t;
    i32 n_ff_a, n_ff_t, n_vocab, n_layer_a, n_layer_t;
    i32 ctx_a_pad, ctx_t_pad;

    WvVec conv1_w, conv1_b, conv2_w, conv2_b, e_pos, e_ln_w, e_ln_b;
    WvMat tok_emb;
    WvVec d_pos, d_ln_w, d_ln_b;

    WvLayer *enc;
    WvLayer *dec;

    bool encoded;
};

static const char *wv_lib_names[] = {
#if defined(__APPLE__)
    "libvulkan.1.dylib", "libvulkan.dylib",
    "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib",
    "libMoltenVK.dylib",
#elif defined(_WIN32)
    "vulkan-1.dll",
#else
    "libvulkan.so.1", "libvulkan.so",
#endif
};

#define WV_LOAD_INSTANCE(f) \
    a->f = (PFN_vk##f)a->GetInstanceProcAddr(inst, "vk" #f)
#define WV_LOAD_DEVICE(f) \
    a->f = (PFN_vk##f)a->GetDeviceProcAddr(dev, "vk" #f)

static bool wv_open_loader(WvApi *a) {
    memset(a, 0, sizeof *a);
    for (usize i = 0; i < sizeof wv_lib_names / sizeof wv_lib_names[0]; i++) {
        a->lib = dlopen(wv_lib_names[i], RTLD_NOW | RTLD_LOCAL);
        if (a->lib) break;
    }
    if (!a->lib) return false;
    a->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(a->lib, "vkGetInstanceProcAddr");
    if (!a->GetInstanceProcAddr) { dlclose(a->lib); a->lib = NULL; return false; }
    a->CreateInstance = (PFN_vkCreateInstance)a->GetInstanceProcAddr(NULL, "vkCreateInstance");
    return a->CreateInstance != NULL;
}

static void wv_load_instance_fns(WvApi *a, VkInstance inst) {
    WV_LOAD_INSTANCE(DestroyInstance);
    WV_LOAD_INSTANCE(EnumeratePhysicalDevices);
    WV_LOAD_INSTANCE(GetPhysicalDeviceProperties);
    WV_LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
    WV_LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    WV_LOAD_INSTANCE(CreateDevice);
    WV_LOAD_INSTANCE(GetDeviceProcAddr);
}

static void wv_load_device_fns(WvApi *a, VkDevice dev) {
    WV_LOAD_DEVICE(DestroyDevice);
    WV_LOAD_DEVICE(GetDeviceQueue);
    WV_LOAD_DEVICE(CreateBuffer);
    WV_LOAD_DEVICE(DestroyBuffer);
    WV_LOAD_DEVICE(GetBufferMemoryRequirements);
    WV_LOAD_DEVICE(AllocateMemory);
    WV_LOAD_DEVICE(FreeMemory);
    WV_LOAD_DEVICE(BindBufferMemory);
    WV_LOAD_DEVICE(MapMemory);
    WV_LOAD_DEVICE(UnmapMemory);
    WV_LOAD_DEVICE(CreateShaderModule);
    WV_LOAD_DEVICE(DestroyShaderModule);
    WV_LOAD_DEVICE(CreateDescriptorSetLayout);
    WV_LOAD_DEVICE(DestroyDescriptorSetLayout);
    WV_LOAD_DEVICE(CreatePipelineLayout);
    WV_LOAD_DEVICE(DestroyPipelineLayout);
    WV_LOAD_DEVICE(CreateComputePipelines);
    WV_LOAD_DEVICE(DestroyPipeline);
    WV_LOAD_DEVICE(CreateDescriptorPool);
    WV_LOAD_DEVICE(DestroyDescriptorPool);
    WV_LOAD_DEVICE(ResetDescriptorPool);
    WV_LOAD_DEVICE(AllocateDescriptorSets);
    WV_LOAD_DEVICE(UpdateDescriptorSets);
    WV_LOAD_DEVICE(CreateCommandPool);
    WV_LOAD_DEVICE(DestroyCommandPool);
    WV_LOAD_DEVICE(AllocateCommandBuffers);
    WV_LOAD_DEVICE(BeginCommandBuffer);
    WV_LOAD_DEVICE(EndCommandBuffer);
    WV_LOAD_DEVICE(ResetCommandBuffer);
    WV_LOAD_DEVICE(CmdBindPipeline);
    WV_LOAD_DEVICE(CmdBindDescriptorSets);
    WV_LOAD_DEVICE(CmdPushConstants);
    WV_LOAD_DEVICE(CmdDispatch);
    WV_LOAD_DEVICE(CmdPipelineBarrier);
    WV_LOAD_DEVICE(CmdCopyBuffer);
    WV_LOAD_DEVICE(CmdFillBuffer);
    WV_LOAD_DEVICE(QueueSubmit);
    WV_LOAD_DEVICE(QueueWaitIdle);
    WV_LOAD_DEVICE(DeviceWaitIdle);
}

/* Create an instance. MoltenVK is a portability driver, so on macOS the
 * enumeration flag and its extension are required or no device is listed;
 * elsewhere asking for them fails, hence the retry. */
static VkInstance wv_create_instance(WvApi *a) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Liu";
    app.apiVersion = VK_API_VERSION_1_0;

    const char *portability[] = { "VK_KHR_portability_enumeration" };

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = portability;
    ci.flags = 0x00000001;   /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */

    VkInstance inst = VK_NULL_HANDLE;
    if (a->CreateInstance(&ci, NULL, &inst) == VK_SUCCESS) return inst;

    ci.enabledExtensionCount = 0;
    ci.ppEnabledExtensionNames = NULL;
    ci.flags = 0;
    if (a->CreateInstance(&ci, NULL, &inst) == VK_SUCCESS) return inst;
    return VK_NULL_HANDLE;
}

bool whisper_vulkan_available(void) {
    WvApi a;
    if (!wv_open_loader(&a)) return false;
    VkInstance inst = wv_create_instance(&a);
    if (!inst) { dlclose(a.lib); return false; }
    wv_load_instance_fns(&a, inst);
    u32 n = 0;
    bool ok = a.EnumeratePhysicalDevices && a.EnumeratePhysicalDevices(inst, &n, NULL) == VK_SUCCESS && n > 0;
    if (a.DestroyInstance) a.DestroyInstance(inst, NULL);
    dlclose(a.lib);
    return ok;
}

/* ---- memory + buffers --------------------------------------------------- */

static i32 wv_find_mem(WhisperVulkan *v, u32 type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    v->api.GetPhysicalDeviceMemoryProperties(v->phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return (i32)i;
    }
    return -1;
}

static void wv_buf_free(WhisperVulkan *v, WvBuf *b) {
    if (b->mapped && v->api.UnmapMemory) v->api.UnmapMemory(v->device, b->mem);
    if (b->buf) v->api.DestroyBuffer(v->device, b->buf, NULL);
    if (b->mem) v->api.FreeMemory(v->device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* `host_visible` asks for a mappable allocation. For the big weight buffer it
 * is a preference, not a requirement: a discrete GPU with no host-visible
 * device-local heap gets device-local memory and a staged copy instead. */
static bool wv_buf_create(WhisperVulkan *v, VkDeviceSize size, bool host_visible,
                          WvBuf *out) {
    memset(out, 0, sizeof *out);
    if (size == 0) size = 256;

    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (v->api.CreateBuffer(v->device, &bi, NULL, &out->buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    v->api.GetBufferMemoryRequirements(v->device, out->buf, &req);

    i32 type = -1;
    if (host_visible) {
        type = wv_find_mem(v, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else {
        /* Prefer a mappable device-local heap when the device has one
         * (integrated GPUs, and desktops with resizable BAR): it removes the
         * staging copy for a half-gigabyte of weights entirely. */
        type = wv_find_mem(v, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0)
            type = wv_find_mem(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (type < 0) { v->api.DestroyBuffer(v->device, out->buf, NULL); out->buf = 0; return false; }

    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (u32)type;
    if (v->api.AllocateMemory(v->device, &ai, NULL, &out->mem) != VK_SUCCESS) {
        v->api.DestroyBuffer(v->device, out->buf, NULL);
        out->buf = 0;
        return false;
    }
    if (v->api.BindBufferMemory(v->device, out->buf, out->mem, 0) != VK_SUCCESS) {
        wv_buf_free(v, out);
        return false;
    }
    out->size = size;

    /* Map once and keep it: these live as long as the backend does. */
    VkPhysicalDeviceMemoryProperties mp;
    v->api.GetPhysicalDeviceMemoryProperties(v->phys, &mp);
    if (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (v->api.MapMemory(v->device, out->mem, 0, VK_WHOLE_SIZE, 0, &out->mapped) != VK_SUCCESS)
            out->mapped = NULL;
    }
    return true;
}

/* One-shot command buffer, submitted and waited on. Used for the weight
 * upload and the per-encode zero fills. */
static bool wv_run_immediate(WhisperVulkan *v, void (*record)(WhisperVulkan *, VkCommandBuffer, void *),
                             void *user) {
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    v->api.ResetCommandBuffer(v->cmd, 0);
    if (v->api.BeginCommandBuffer(v->cmd, &bi) != VK_SUCCESS) return false;
    record(v, v->cmd, user);
    if (v->api.EndCommandBuffer(v->cmd) != VK_SUCCESS) return false;

    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v->cmd;
    if (v->api.QueueSubmit(v->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    return v->api.QueueWaitIdle(v->queue) == VK_SUCCESS;
}

typedef struct { WvBuf *dst; VkDeviceSize dst_off, size; } WvCopyJob;

static void wv_record_copy(WhisperVulkan *v, VkCommandBuffer cb, void *user) {
    WvCopyJob *j = user;
    VkBufferCopy region;
    region.srcOffset = 0;
    region.dstOffset = j->dst_off;
    region.size = j->size;
    v->api.CmdCopyBuffer(cb, v->host.buf, j->dst->buf, 1, &region);
}

/* Write `n` bytes into `dst` at `off`, mapped when possible and staged in
 * WV_STAGE_BYTES chunks when not. */
static bool wv_write(WhisperVulkan *v, WvBuf *dst, VkDeviceSize off,
                     const void *src, VkDeviceSize n) {
    if (n == 0) return true;
    if (dst->mapped) {
        memcpy((u8 *)dst->mapped + off, src, (usize)n);
        return true;
    }
    if (!v->host.mapped) return false;
    const u8 *p = src;
    while (n > 0) {
        VkDeviceSize chunk = n < v->host.size ? n : v->host.size;
        memcpy(v->host.mapped, p, (usize)chunk);
        WvCopyJob j = { dst, off, chunk };
        if (!wv_run_immediate(v, wv_record_copy, &j)) return false;
        p += chunk;
        off += chunk;
        n -= chunk;
    }
    return true;
}

typedef struct { WvBuf *b; } WvZeroJob;

static void wv_record_zero(WhisperVulkan *v, VkCommandBuffer cb, void *user) {
    WvZeroJob *j = user;
    v->api.CmdFillBuffer(cb, j->b->buf, 0, VK_WHOLE_SIZE, 0);
}

static bool wv_zero(WhisperVulkan *v, WvBuf *b) {
    if (b->mapped) { memset(b->mapped, 0, (usize)b->size); return true; }
    WvZeroJob j = { b };
    return wv_run_immediate(v, wv_record_zero, &j);
}

/* ---- pipelines ---------------------------------------------------------- */

typedef struct { const u32 *spv; usize bytes; } WvSpv;

static WvSpv wv_spv_for(WvPipe p) {
    switch (p) {
        case WV_PIPE_GEMM:          return (WvSpv){ WV_SPV_gemm,          sizeof WV_SPV_gemm };
        case WV_PIPE_MATVEC:        return (WvSpv){ WV_SPV_matvec,        sizeof WV_SPV_matvec };
        case WV_PIPE_CONV1D:        return (WvSpv){ WV_SPV_conv1d,        sizeof WV_SPV_conv1d };
        case WV_PIPE_LAYERNORM:     return (WvSpv){ WV_SPV_layernorm,     sizeof WV_SPV_layernorm };
        case WV_PIPE_ATTENTION:     return (WvSpv){ WV_SPV_attention,     sizeof WV_SPV_attention };
        case WV_PIPE_ELEMENTWISE:   return (WvSpv){ WV_SPV_elementwise,   sizeof WV_SPV_elementwise };
        case WV_PIPE_TRANSPOSE_POS: return (WvSpv){ WV_SPV_transpose_pos, sizeof WV_SPV_transpose_pos };
        case WV_PIPE_EMBED:         return (WvSpv){ WV_SPV_embed,         sizeof WV_SPV_embed };
        case WV_PIPE_KVSTORE:       return (WvSpv){ WV_SPV_kvstore,       sizeof WV_SPV_kvstore };
        default:                    return (WvSpv){ NULL, 0 };
    }
}

static bool wv_build_pipelines(WhisperVulkan *v) {
    VkDescriptorSetLayoutBinding binds[WV_BINDINGS];
    memset(binds, 0, sizeof binds);
    for (u32 i = 0; i < WV_BINDINGS; i++) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = WV_BINDINGS;
    li.pBindings = binds;
    if (v->api.CreateDescriptorSetLayout(v->device, &li, NULL, &v->set_layout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pcr;
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 32;      /* every param block here is <= 32 bytes */

    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &v->set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (v->api.CreatePipelineLayout(v->device, &pli, NULL, &v->pipe_layout) != VK_SUCCESS)
        return false;

    for (i32 i = 0; i < WV_PIPE_COUNT; i++) {
        WvSpv s = wv_spv_for((WvPipe)i);
        if (!s.spv) return false;

        VkShaderModuleCreateInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = s.bytes;
        mi.pCode = s.spv;
        if (v->api.CreateShaderModule(v->device, &mi, NULL, &v->modules[i]) != VK_SUCCESS)
            return false;

        VkComputePipelineCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = v->modules[i];
        ci.stage.pName = "main";
        ci.layout = v->pipe_layout;
        if (v->api.CreateComputePipelines(v->device, VK_NULL_HANDLE, 1, &ci, NULL,
                                          &v->pipes[i]) != VK_SUCCESS)
            return false;
    }

    VkDescriptorPoolSize ps;
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = WV_MAX_SETS * WV_BINDINGS;
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = WV_MAX_SETS;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (v->api.CreateDescriptorPool(v->device, &dpi, NULL, &v->desc_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayout *layouts = malloc(WV_MAX_SETS * sizeof *layouts);
    if (!layouts) return false;
    for (i32 i = 0; i < WV_MAX_SETS; i++) layouts[i] = v->set_layout;
    VkDescriptorSetAllocateInfo dai;
    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = v->desc_pool;
    dai.descriptorSetCount = WV_MAX_SETS;
    dai.pSetLayouts = layouts;
    bool ok = v->api.AllocateDescriptorSets(v->device, &dai, v->sets) == VK_SUCCESS;
    free(layouts);
    return ok;
}

/* ---- dispatch ----------------------------------------------------------- */

/* Bind all four buffers, using a valid dummy for unused slots because every
 * declared binding must be written. Push parameters, dispatch, then issue a
 * barrier.
 *
 * The barrier is unconditional because Vulkan does not order dispatches
 * implicitly and most kernels consume the preceding kernel's output. This
 * keeps synchronization correct across the dispatch graph. */
static void wv_dispatch(WhisperVulkan *v, WvPipe pipe,
                        WvBuf *b0, VkDeviceSize o0,
                        WvBuf *b1, VkDeviceSize o1,
                        WvBuf *b2, VkDeviceSize o2,
                        WvBuf *b3, VkDeviceSize o3,
                        const void *push, usize push_size,
                        u32 gx, u32 gy, u32 gz) {
    if (v->set_next >= WV_MAX_SETS) return;    /* checked by the caller */
    VkDescriptorSet set = v->sets[v->set_next++];

    WvBuf *bufs[WV_BINDINGS] = { b0, b1, b2, b3 };
    VkDeviceSize offs[WV_BINDINGS] = { o0, o1, o2, o3 };
    VkDescriptorBufferInfo info[WV_BINDINGS];
    VkWriteDescriptorSet w[WV_BINDINGS];
    memset(info, 0, sizeof info);
    memset(w, 0, sizeof w);

    for (u32 i = 0; i < WV_BINDINGS; i++) {
        WvBuf *b = bufs[i] ? bufs[i] : &v->host;
        info[i].buffer = b->buf;
        info[i].offset = bufs[i] ? offs[i] : 0;
        info[i].range  = VK_WHOLE_SIZE;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &info[i];
    }
    v->api.UpdateDescriptorSets(v->device, WV_BINDINGS, w, 0, NULL);

    v->api.CmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipes[pipe]);
    v->api.CmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 v->pipe_layout, 0, 1, &set, 0, NULL);
    if (push && push_size)
        v->api.CmdPushConstants(v->cmd, v->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                0, (u32)push_size, push);
    v->api.CmdDispatch(v->cmd, gx, gy, gz);

    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    v->api.CmdPipelineBarrier(v->cmd,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0, 1, &mb, 0, NULL, 0, NULL);
}

/* ---- weight packing ----------------------------------------------------- */

/* Two passes over the tensor list, one to measure and one to copy, so the
 * layout decision (256-byte alignment per tensor) lives in one place. */
typedef struct { u8 *base; u64 cursor; bool ok; } WvPack;

static u64 wv_align(u64 x) { return (x + 255ull) & ~255ull; }

static WvMat wv_pack_mat(WvPack *p, const WhisperTensor *t) {
    WvMat m = { 0, 0, 0, 0 };
    if (!p->ok || !t) { p->ok = false; return m; }
    if (whisper_type_block(t->dtype) == 0 || t->ne[0] % 32 != 0) { p->ok = false; return m; }
    p->cursor = wv_align(p->cursor);
    m.off = p->cursor;
    m.dtype = t->dtype;
    m.K = t->ne[0];
    m.N = t->ne[1];
    if (p->base) memcpy(p->base + m.off, t->data, (usize)t->nbytes);
    p->cursor += t->nbytes;
    return m;
}

static WvVec wv_pack_vec(WvPack *p, const WhisperTensor *t) {
    WvVec v = { 0, 0 };
    if (!p->ok || !t) { p->ok = false; return v; }
    i64 n = 1;
    for (i32 d = 0; d < t->n_dims; d++) n *= t->ne[d];
    p->cursor = wv_align(p->cursor);
    v.off = p->cursor / sizeof(f32);
    v.n = (i32)n;
    if (p->base) {
        if (!whisper_dequant_row(t->data, t->dtype, n, (f32 *)(p->base + p->cursor)))
            p->ok = false;
    }
    p->cursor += (u64)n * sizeof(f32);
    return v;
}

static void wv_pack_layer(WvPack *w, WvPack *s, const WhisperLayer *L,
                          WvLayer *out, bool with_cross) {
    out->attn_ln_w = wv_pack_vec(s, L->attn_ln_w);
    out->attn_ln_b = wv_pack_vec(s, L->attn_ln_b);
    out->attn_q_w  = wv_pack_mat(w, L->attn_q_w);
    out->attn_q_b  = wv_pack_vec(s, L->attn_q_b);
    out->attn_k_w  = wv_pack_mat(w, L->attn_k_w);
    out->attn_v_w  = wv_pack_mat(w, L->attn_v_w);
    out->attn_v_b  = wv_pack_vec(s, L->attn_v_b);
    out->attn_o_w  = wv_pack_mat(w, L->attn_o_w);
    out->attn_o_b  = wv_pack_vec(s, L->attn_o_b);

    if (with_cross) {
        out->cross_ln_w = wv_pack_vec(s, L->cross_ln_w);
        out->cross_ln_b = wv_pack_vec(s, L->cross_ln_b);
        out->cross_q_w  = wv_pack_mat(w, L->cross_q_w);
        out->cross_q_b  = wv_pack_vec(s, L->cross_q_b);
        out->cross_k_w  = wv_pack_mat(w, L->cross_k_w);
        out->cross_v_w  = wv_pack_mat(w, L->cross_v_w);
        out->cross_v_b  = wv_pack_vec(s, L->cross_v_b);
        out->cross_o_w  = wv_pack_mat(w, L->cross_o_w);
        out->cross_o_b  = wv_pack_vec(s, L->cross_o_b);
    }

    out->mlp_ln_w = wv_pack_vec(s, L->mlp_ln_w);
    out->mlp_ln_b = wv_pack_vec(s, L->mlp_ln_b);
    out->mlp_0_w  = wv_pack_mat(w, L->mlp_0_w);
    out->mlp_0_b  = wv_pack_vec(s, L->mlp_0_b);
    out->mlp_2_w  = wv_pack_mat(w, L->mlp_2_w);
    out->mlp_2_b  = wv_pack_vec(s, L->mlp_2_b);
}

static void wv_pack_all(WhisperVulkan *v, const WhisperWeights *W,
                        WvPack *w, WvPack *s) {
    v->conv1_w = wv_pack_vec(s, W->e_conv1_w);
    v->conv1_b = wv_pack_vec(s, W->e_conv1_b);
    v->conv2_w = wv_pack_vec(s, W->e_conv2_w);
    v->conv2_b = wv_pack_vec(s, W->e_conv2_b);
    v->e_pos   = wv_pack_vec(s, W->e_pos);
    v->e_ln_w  = wv_pack_vec(s, W->e_ln_w);
    v->e_ln_b  = wv_pack_vec(s, W->e_ln_b);

    for (i32 il = 0; il < v->n_layer_a; il++)
        wv_pack_layer(w, s, &W->enc[il], &v->enc[il], false);

    v->tok_emb = wv_pack_mat(w, W->d_tok_emb);
    v->d_pos   = wv_pack_vec(s, W->d_pos);
    v->d_ln_w  = wv_pack_vec(s, W->d_ln_w);
    v->d_ln_b  = wv_pack_vec(s, W->d_ln_b);

    for (i32 il = 0; il < v->n_layer_t; il++)
        wv_pack_layer(w, s, &W->dec[il], &v->dec[il], true);
}

/* ---- create / free ------------------------------------------------------ */

void whisper_vulkan_free(WhisperVulkan *v) {
    if (!v) return;
    if (v->device) {
        if (v->api.DeviceWaitIdle) v->api.DeviceWaitIdle(v->device);
        wv_buf_free(v, &v->wbuf); wv_buf_free(v, &v->sbuf);
        wv_buf_free(v, &v->mel); wv_buf_free(v, &v->ff);
        wv_buf_free(v, &v->x); wv_buf_free(v, &v->xn);
        wv_buf_free(v, &v->q); wv_buf_free(v, &v->k);
        wv_buf_free(v, &v->v); wv_buf_free(v, &v->att);
        wv_buf_free(v, &v->cross_k); wv_buf_free(v, &v->cross_v);
        wv_buf_free(v, &v->self_k); wv_buf_free(v, &v->self_v);
        wv_buf_free(v, &v->ids); wv_buf_free(v, &v->logits);
        wv_buf_free(v, &v->host);

        for (i32 i = 0; i < WV_PIPE_COUNT; i++) {
            if (v->pipes[i]) v->api.DestroyPipeline(v->device, v->pipes[i], NULL);
            if (v->modules[i]) v->api.DestroyShaderModule(v->device, v->modules[i], NULL);
        }
        if (v->desc_pool) v->api.DestroyDescriptorPool(v->device, v->desc_pool, NULL);
        if (v->pipe_layout) v->api.DestroyPipelineLayout(v->device, v->pipe_layout, NULL);
        if (v->set_layout) v->api.DestroyDescriptorSetLayout(v->device, v->set_layout, NULL);
        if (v->cmd_pool) v->api.DestroyCommandPool(v->device, v->cmd_pool, NULL);
        v->api.DestroyDevice(v->device, NULL);
    }
    if (v->instance && v->api.DestroyInstance) v->api.DestroyInstance(v->instance, NULL);
    if (v->api.lib) dlclose(v->api.lib);
    free(v->enc);
    free(v->dec);
    free(v);
}

const char *whisper_vulkan_device_name(const WhisperVulkan *v) {
    return v ? v->device_name : "";
}

WhisperVulkan *whisper_vulkan_create(const WhisperWeights *W, char *err, usize err_cap) {
    if (!W) return NULL;
    if (err && err_cap) err[0] = '\0';

    WhisperVulkan *v = calloc(1, sizeof *v);
    if (!v) { if (err && err_cap) snprintf(err, err_cap, "out of memory"); return NULL; }

#define WV_FAIL(msg) do { if (err && err_cap) snprintf(err, err_cap, "%s", msg); \
                          whisper_vulkan_free(v); return NULL; } while (0)

    if (!wv_open_loader(&v->api)) WV_FAIL("no Vulkan loader");
    v->instance = wv_create_instance(&v->api);
    if (!v->instance) WV_FAIL("cannot create a Vulkan instance");
    wv_load_instance_fns(&v->api, v->instance);

    u32 n_dev = 0;
    if (v->api.EnumeratePhysicalDevices(v->instance, &n_dev, NULL) != VK_SUCCESS || n_dev == 0)
        WV_FAIL("no Vulkan device");
    if (n_dev > 8) n_dev = 8;
    VkPhysicalDevice devs[8];
    v->api.EnumeratePhysicalDevices(v->instance, &n_dev, devs);

    /* Prefer a discrete GPU: on a laptop with both, the integrated one is
     * usually also the display device and the slower of the two. */
    i32 best = -1, best_score = -1;
    u32 best_family = 0;
    for (u32 i = 0; i < n_dev; i++) {
        u32 nq = 0;
        v->api.GetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        if (nq == 0 || nq > 32) continue;
        VkQueueFamilyProperties qf[32];
        v->api.GetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        i32 family = -1;
        for (u32 f = 0; f < nq; f++)
            if (qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = (i32)f; break; }
        if (family < 0) continue;

        VkPhysicalDeviceProperties pr;
        v->api.GetPhysicalDeviceProperties(devs[i], &pr);
        i32 score = (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   ? 3
                  : (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 2
                  : 1;
        if (score > best_score) {
            best_score = score;
            best = (i32)i;
            best_family = (u32)family;
            snprintf(v->device_name, sizeof v->device_name, "%s", pr.deviceName);
        }
    }
    if (best < 0) WV_FAIL("no Vulkan device with a compute queue");
    v->phys = devs[best];
    v->queue_family = best_family;

    const f32 prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = v->queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    /* MoltenVK is a portability implementation and requires the subset
     * extension to be enabled on the device; asking for it elsewhere fails,
     * so the create is retried without. */
    const char *portability_dev[] = { "VK_KHR_portability_subset" };
    VkDeviceCreateInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = portability_dev;
    if (v->api.CreateDevice(v->phys, &di, NULL, &v->device) != VK_SUCCESS) {
        di.enabledExtensionCount = 0;
        di.ppEnabledExtensionNames = NULL;
        if (v->api.CreateDevice(v->phys, &di, NULL, &v->device) != VK_SUCCESS)
            WV_FAIL("cannot create a Vulkan device");
    }
    wv_load_device_fns(&v->api, v->device);
    v->api.GetDeviceQueue(v->device, v->queue_family, 0, &v->queue);

    VkCommandPoolCreateInfo cpi;
    memset(&cpi, 0, sizeof cpi);
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = v->queue_family;
    if (v->api.CreateCommandPool(v->device, &cpi, NULL, &v->cmd_pool) != VK_SUCCESS)
        WV_FAIL("cannot create a command pool");

    VkCommandBufferAllocateInfo cbi;
    memset(&cbi, 0, sizeof cbi);
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = v->cmd_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (v->api.AllocateCommandBuffers(v->device, &cbi, &v->cmd) != VK_SUCCESS)
        WV_FAIL("cannot allocate a command buffer");

    if (!wv_build_pipelines(v)) WV_FAIL("cannot build the Vulkan compute pipelines");

    /* ---- dimensions ---- */
    v->n_state   = W->h.n_audio_state;
    v->n_ctx_a   = W->h.n_audio_ctx;
    v->n_ctx_t   = W->h.n_text_ctx;
    v->n_mels    = W->h.n_mels;
    v->n_head_a  = W->h.n_audio_head;
    v->n_head_t  = W->h.n_text_head;
    v->hd_a      = W->n_audio_head_dim;
    v->hd_t      = W->n_text_head_dim;
    v->n_ff_a    = W->n_audio_ff;
    v->n_ff_t    = W->n_text_ff;
    v->n_vocab   = W->h.n_vocab;
    v->n_layer_a = W->h.n_audio_layer;
    v->n_layer_t = W->h.n_text_layer;

    if (v->n_ctx_a * 2 != WHISPER_N_FRAMES) WV_FAIL("unexpected audio context length");
    /* attention.comp stages a QB x head_dim output tile in shared memory. */
    if (v->hd_a > 64 || v->hd_t > 64) WV_FAIL("head dimension unsupported by the Vulkan kernel");

    /* Padded to whole GEMM and attention tiles for the same reason as the
     * Metal backend: the kernels round their grids up and a ragged tail would
     * otherwise touch rows the buffer does not have. */
    i32 pad = WV_BM > WV_ATTN_QB ? WV_BM : WV_ATTN_QB;
    v->ctx_a_pad = ((v->n_ctx_a + pad - 1) / pad) * pad;
    v->ctx_t_pad = ((v->n_ctx_t + pad - 1) / pad) * pad;

    v->enc = calloc((usize)v->n_layer_a, sizeof *v->enc);
    v->dec = calloc((usize)v->n_layer_t, sizeof *v->dec);
    if (!v->enc || !v->dec) WV_FAIL("out of memory");

    WvPack wp = { NULL, 0, true }, sp = { NULL, 0, true };
    wv_pack_all(v, W, &wp, &sp);
    if (!wp.ok || !sp.ok) WV_FAIL("unsupported weight layout for the Vulkan backend");

    if (!wv_buf_create(v, WV_STAGE_BYTES, true, &v->host))
        WV_FAIL("cannot allocate a staging buffer");
    /* The dequant shader reads whole 32-bit words (wqByte: data[off >> 2]),
     * and a Q4/Q5 matrix's byte count need not be a multiple of four, so the
     * last word of the final tensor would straddle the end of an exact-size
     * buffer. Pad both to the packer's own alignment. */
    wp.cursor = wv_align(wp.cursor);
    sp.cursor = wv_align(sp.cursor);
    if (!wv_buf_create(v, wp.cursor, false, &v->wbuf) ||
        !wv_buf_create(v, sp.cursor, false, &v->sbuf))
        WV_FAIL("cannot allocate GPU memory for the model weights");

    /* Pass 2: build the payloads on the host, then hand them to the device.
     * The staging path needs a contiguous source, so both are assembled in
     * ordinary memory first and freed straight after. */
    {
        u8 *wtmp = malloc((usize)wp.cursor);
        u8 *stmp = malloc((usize)sp.cursor);
        if (!wtmp || !stmp) { free(wtmp); free(stmp); WV_FAIL("out of memory"); }
        WvPack w2 = { wtmp, 0, true }, s2 = { stmp, 0, true };
        wv_pack_all(v, W, &w2, &s2);
        bool ok = w2.ok && s2.ok &&
                  wv_write(v, &v->wbuf, 0, wtmp, wp.cursor) &&
                  wv_write(v, &v->sbuf, 0, stmp, sp.cursor);
        free(wtmp);
        free(stmp);
        if (!ok) WV_FAIL("cannot upload the model weights");
    }

    const VkDeviceSize ns = (VkDeviceSize)v->n_state * sizeof(f32);
    const VkDeviceSize ta = (VkDeviceSize)v->ctx_a_pad;
    const VkDeviceSize tt = (VkDeviceSize)v->ctx_t_pad;
    usize ff_bytes;
    if (!whisper_scratch_bytes(v->ctx_a_pad, v->ctx_t_pad, v->n_ff_a,
        v->n_ff_t, v->n_state, WHISPER_N_FRAMES, &ff_bytes))
        WV_FAIL("convolution/MLP scratch size overflow");

    bool ok = wv_buf_create(v, (VkDeviceSize)v->n_mels * WHISPER_N_FRAMES * sizeof(f32), true, &v->mel)
           && wv_buf_create(v, (VkDeviceSize)ff_bytes, false, &v->ff)
           && wv_buf_create(v, ta * ns, false, &v->x)
           && wv_buf_create(v, ta * ns, false, &v->xn)
           && wv_buf_create(v, ta * ns, false, &v->q)
           && wv_buf_create(v, ta * ns, false, &v->k)
           && wv_buf_create(v, ta * ns, false, &v->v)
           && wv_buf_create(v, ta * ns, false, &v->att)
           && wv_buf_create(v, (VkDeviceSize)v->n_layer_t * ta * ns, false, &v->cross_k)
           && wv_buf_create(v, (VkDeviceSize)v->n_layer_t * ta * ns, false, &v->cross_v)
           && wv_buf_create(v, (VkDeviceSize)v->n_layer_t * tt * ns, false, &v->self_k)
           && wv_buf_create(v, (VkDeviceSize)v->n_layer_t * tt * ns, false, &v->self_v)
           && wv_buf_create(v, (VkDeviceSize)v->n_ctx_t * sizeof(i32), true, &v->ids)
           && wv_buf_create(v, (VkDeviceSize)v->n_vocab * sizeof(f32), true, &v->logits);
    if (!ok) WV_FAIL("cannot allocate GPU work buffers");

    /* Zero once: padded rows are masked out of the softmax so their scores
     * never matter, but their probability is 0 and 0 * NaN is NaN — an
     * uninitialized V row would poison the output. */
    WvBuf *zero[] = { &v->ff, &v->x, &v->xn, &v->q, &v->k, &v->v, &v->att,
                      &v->cross_k, &v->cross_v, &v->self_k, &v->self_v };
    for (usize i = 0; i < sizeof zero / sizeof zero[0]; i++)
        if (!wv_zero(v, zero[i])) WV_FAIL("cannot clear GPU work buffers");

#undef WV_FAIL
    return v;
}

void whisper_vulkan_decode_reset(WhisperVulkan *v) { (void)v; /* position-indexed */ }

/* ---- graph helpers ------------------------------------------------------ */

static bool wv_begin(WhisperVulkan *v) {
    v->set_next = 0;
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    v->api.ResetCommandBuffer(v->cmd, 0);
    return v->api.BeginCommandBuffer(v->cmd, &bi) == VK_SUCCESS;
}

static bool wv_submit(WhisperVulkan *v) {
    if (v->api.EndCommandBuffer(v->cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v->cmd;
    if (v->api.QueueSubmit(v->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    return v->api.QueueWaitIdle(v->queue) == VK_SUCCESS;
}

static VkDeviceSize wv_soff(WvVec x) { return (VkDeviceSize)x.off * sizeof(f32); }

static void wv_norm(WhisperVulkan *v, WvBuf *out, WvBuf *in, WvVec g, WvVec b, i32 n_tokens) {
    struct WmNorm p = { v->n_state, n_tokens, 0, 0 };
    wv_dispatch(v, WV_PIPE_LAYERNORM,
                out, 0, in, 0, &v->sbuf, wv_soff(g), &v->sbuf, wv_soff(b),
                &p, sizeof p, (u32)n_tokens, 1, 1);
}

static void wv_mul(WhisperVulkan *v, WvBuf *out, VkDeviceSize out_off,
                   WvBuf *in, VkDeviceSize in_off,
                   WvMat W, WvVec bias, bool has_bias, i32 M) {
    struct WmGemm p = { M, W.N, W.K, W.dtype, has_bias ? 1 : 0, 0, 0, 0 };
    const bool tiny = (M <= 4);
    wv_dispatch(v, tiny ? WV_PIPE_MATVEC : WV_PIPE_GEMM,
                in, in_off,
                &v->wbuf, (VkDeviceSize)W.off,
                &v->sbuf, has_bias ? wv_soff(bias) : 0,
                out, out_off,
                &p, sizeof p,
                tiny ? (u32)W.N : (u32)((W.N + WV_BN - 1) / WV_BN),
                tiny ? 1u      : (u32)((M + WV_BM - 1) / WV_BM),
                1);
}

static void wv_attn(WhisperVulkan *v, WvBuf *kb, VkDeviceSize k_off,
                    WvBuf *vb, VkDeviceSize v_off,
                    i32 n_q, i32 n_kv, i32 n_head, i32 hd,
                    bool causal, i32 causal_offset) {
    struct WmAttn p = { n_q, n_kv, n_head, hd, v->n_state,
                        causal ? 1 : 0, causal_offset, 1.0f / sqrtf((f32)hd) };
    wv_dispatch(v, WV_PIPE_ATTENTION,
                &v->q, 0, kb, k_off, vb, v_off, &v->att, 0,
                &p, sizeof p,
                (u32)((n_q + WV_ATTN_QB - 1) / WV_ATTN_QB), (u32)n_head, 1);
}

static void wv_add(WhisperVulkan *v, WvBuf *x, WvBuf *y, i32 n) {
    struct WvElem p = { n, 0, 0, 0 };
    wv_dispatch(v, WV_PIPE_ELEMENTWISE, x, 0, y, 0, NULL, 0, NULL, 0,
                &p, sizeof p, (u32)((n + 255) / 256), 1, 1);
}

static void wv_gelu(WhisperVulkan *v, WvBuf *x, i32 n) {
    struct WvElem p = { n, 1, 0, 0 };
    wv_dispatch(v, WV_PIPE_ELEMENTWISE, x, 0, NULL, 0, NULL, 0, NULL, 0,
                &p, sizeof p, (u32)((n + 255) / 256), 1, 1);
}

/* ---- encode ------------------------------------------------------------- */

bool whisper_vulkan_encode(WhisperVulkan *v, const f32 *mel) {
    if (!v || !mel) return false;
    if (!v->mel.mapped) return false;
    memcpy(v->mel.mapped, mel, (usize)v->n_mels * WHISPER_N_FRAMES * sizeof(f32));

    if (!wv_begin(v)) return false;

    const i32 T = v->n_ctx_a;
    const i32 ns = v->n_state;

    /* conv1: mel -> ff (also the convolution scratch), stride 1, GELU. */
    {
        struct WmConv p = { v->n_mels, ns, WHISPER_N_FRAMES, WHISPER_N_FRAMES, 1, 1, 0, 0 };
        wv_dispatch(v, WV_PIPE_CONV1D,
                    &v->mel, 0, &v->sbuf, wv_soff(v->conv1_w),
                    &v->sbuf, wv_soff(v->conv1_b), &v->ff, 0,
                    &p, sizeof p,
                    (u32)((WHISPER_N_FRAMES + 255) / 256), (u32)ns, 1);
    }
    /* conv2: ff -> q (free until the first layer), stride 2, GELU. */
    {
        struct WmConv p = { ns, ns, WHISPER_N_FRAMES, T, 2, 1, 0, 0 };
        wv_dispatch(v, WV_PIPE_CONV1D,
                    &v->ff, 0, &v->sbuf, wv_soff(v->conv2_w),
                    &v->sbuf, wv_soff(v->conv2_b), &v->q, 0,
                    &p, sizeof p, (u32)((T + 255) / 256), (u32)ns, 1);
    }
    /* channel-major -> token-major, folding in the positional table. */
    {
        struct WmTranspose p = { ns, T, 0, 0 };
        wv_dispatch(v, WV_PIPE_TRANSPOSE_POS,
                    &v->q, 0, &v->sbuf, wv_soff(v->e_pos), &v->x, 0, NULL, 0,
                    &p, sizeof p, (u32)((T + 31) / 32), (u32)((ns + 7) / 8), 1);
    }

    for (i32 il = 0; il < v->n_layer_a; il++) {
        const WvLayer *L = &v->enc[il];

        wv_norm(v, &v->xn, &v->x, L->attn_ln_w, L->attn_ln_b, T);
        wv_mul(v, &v->q,  0, &v->xn, 0, L->attn_q_w, L->attn_q_b, true,  T);
        wv_mul(v, &v->k,  0, &v->xn, 0, L->attn_k_w, L->attn_q_b, false, T);
        wv_mul(v, &v->v,  0, &v->xn, 0, L->attn_v_w, L->attn_v_b, true,  T);
        wv_attn(v, &v->k, 0, &v->v, 0, T, T, v->n_head_a, v->hd_a, false, 0);
        wv_mul(v, &v->xn, 0, &v->att, 0, L->attn_o_w, L->attn_o_b, true, T);
        wv_add(v, &v->x, &v->xn, T * ns);

        wv_norm(v, &v->xn, &v->x, L->mlp_ln_w, L->mlp_ln_b, T);
        wv_mul(v, &v->ff, 0, &v->xn, 0, L->mlp_0_w, L->mlp_0_b, true, T);
        wv_gelu(v, &v->ff, T * v->n_ff_a);
        wv_mul(v, &v->xn, 0, &v->ff, 0, L->mlp_2_w, L->mlp_2_b, true, T);
        wv_add(v, &v->x, &v->xn, T * ns);
    }

    /* Final norm lands in xn, which is then the encoder output; x is about to
     * be reused by the decoder for token embeddings. */
    wv_norm(v, &v->xn, &v->x, v->e_ln_w, v->e_ln_b, T);

    for (i32 il = 0; il < v->n_layer_t; il++) {
        const WvLayer *L = &v->dec[il];
        const VkDeviceSize off = (VkDeviceSize)il * (VkDeviceSize)v->ctx_a_pad *
                                 (VkDeviceSize)ns * sizeof(f32);
        wv_mul(v, &v->cross_k, off, &v->xn, 0, L->cross_k_w, L->cross_v_b, false, T);
        wv_mul(v, &v->cross_v, off, &v->xn, 0, L->cross_v_w, L->cross_v_b, true,  T);
    }

    if (!wv_submit(v)) return false;
    v->encoded = true;
    return true;
}

/* ---- decode ------------------------------------------------------------- */

bool whisper_vulkan_decode(WhisperVulkan *v, const i32 *tokens, i32 n, i32 n_past,
                           f32 *logits) {
    if (!v || !tokens || n <= 0 || !logits || !v->encoded) return false;
    if (n_past < 0 || n_past + n > v->n_ctx_t) return false;
    for (i32 i = 0; i < n; i++) if (tokens[i] < 0 || tokens[i] >= v->n_vocab) return false;
    if (!v->ids.mapped || !v->logits.mapped) return false;

    memcpy(v->ids.mapped, tokens, (usize)n * sizeof(i32));
    if (!wv_begin(v)) return false;

    const i32 ns = v->n_state;
    const i32 Ta = v->n_ctx_a;

    {
        struct WmEmbed p = { ns, n, n_past, v->tok_emb.dtype };
        wv_dispatch(v, WV_PIPE_EMBED,
                    &v->wbuf, (VkDeviceSize)v->tok_emb.off,
                    &v->ids, 0, &v->sbuf, wv_soff(v->d_pos), &v->x, 0,
                    &p, sizeof p, (u32)((ns + 63) / 64), (u32)((n + 3) / 4), 1);
    }

    for (i32 il = 0; il < v->n_layer_t; il++) {
        const WvLayer *L = &v->dec[il];
        const VkDeviceSize self_off  = (VkDeviceSize)il * (VkDeviceSize)v->ctx_t_pad *
                                       (VkDeviceSize)ns * sizeof(f32);
        const VkDeviceSize cross_off = (VkDeviceSize)il * (VkDeviceSize)v->ctx_a_pad *
                                       (VkDeviceSize)ns * sizeof(f32);

        /* causal self-attention */
        wv_norm(v, &v->xn, &v->x, L->attn_ln_w, L->attn_ln_b, n);
        wv_mul(v, &v->q, 0, &v->xn, 0, L->attn_q_w, L->attn_q_b, true,  n);
        wv_mul(v, &v->k, 0, &v->xn, 0, L->attn_k_w, L->attn_q_b, false, n);
        wv_mul(v, &v->v, 0, &v->xn, 0, L->attn_v_w, L->attn_v_b, true,  n);

        struct WmEmbed kv = { ns, n, n_past, 0 };
        wv_dispatch(v, WV_PIPE_KVSTORE, &v->k, 0, &v->self_k, self_off, NULL, 0, NULL, 0,
                    &kv, sizeof kv, (u32)((ns + 63) / 64), (u32)((n + 3) / 4), 1);
        wv_dispatch(v, WV_PIPE_KVSTORE, &v->v, 0, &v->self_v, self_off, NULL, 0, NULL, 0,
                    &kv, sizeof kv, (u32)((ns + 63) / 64), (u32)((n + 3) / 4), 1);

        wv_attn(v, &v->self_k, self_off, &v->self_v, self_off,
                n, n_past + n, v->n_head_t, v->hd_t, true, n_past);
        wv_mul(v, &v->xn, 0, &v->att, 0, L->attn_o_w, L->attn_o_b, true, n);
        wv_add(v, &v->x, &v->xn, n * ns);

        /* cross-attention */
        wv_norm(v, &v->xn, &v->x, L->cross_ln_w, L->cross_ln_b, n);
        wv_mul(v, &v->q, 0, &v->xn, 0, L->cross_q_w, L->cross_q_b, true, n);
        wv_attn(v, &v->cross_k, cross_off, &v->cross_v, cross_off,
                n, Ta, v->n_head_t, v->hd_t, false, 0);
        wv_mul(v, &v->xn, 0, &v->att, 0, L->cross_o_w, L->cross_o_b, true, n);
        wv_add(v, &v->x, &v->xn, n * ns);

        /* MLP */
        wv_norm(v, &v->xn, &v->x, L->mlp_ln_w, L->mlp_ln_b, n);
        wv_mul(v, &v->ff, 0, &v->xn, 0, L->mlp_0_w, L->mlp_0_b, true, n);
        wv_gelu(v, &v->ff, n * v->n_ff_t);
        wv_mul(v, &v->xn, 0, &v->ff, 0, L->mlp_2_w, L->mlp_2_b, true, n);
        wv_add(v, &v->x, &v->xn, n * ns);
    }

    wv_norm(v, &v->xn, &v->x, v->d_ln_w, v->d_ln_b, n);

    /* Tied LM head, last token only. */
    {
        WvVec none = { 0, 0 };
        wv_mul(v, &v->logits, 0, &v->xn,
               (VkDeviceSize)(n - 1) * (VkDeviceSize)ns * sizeof(f32),
               v->tok_emb, none, false, 1);
    }

    if (!wv_submit(v)) return false;
    memcpy(logits, v->logits.mapped, (usize)v->n_vocab * sizeof(f32));
    return true;
}
