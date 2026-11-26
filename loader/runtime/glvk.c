// The OpenGL ES that the port's NVN side (nvn2.c) draws with, done over a Switch 2's Vulkan by the port itself: each function
// nvn2.c calls has one here of the same name with "vk_" before it (glvk_route.h sends a call to it when nx2_vulkan is set), which
// does what OpenGL says of it as far as nvn2.c uses it, and no further.
//
// Why. The host's own OpenGL (Mesa's, over the same Vulkan) took some 25 microseconds a draw as nvn2.c uses it, most of it in giving
// each draw its uniform blocks; a draw recorded straight into Vulkan takes under one (tools/vktest/vkdraws.c, measured on a
// Switch 2, 2026-11-24). A game draws a thousand things a picture.
//
// How it is laid out. OpenGL's objects are rows of tables here, known by their numbers: textures (an image of Vulkan's, or a view
// of another's), samplers, buffers (pieces of large blocks of memory that stay mapped), framebuffers (which textures are drawn
// to), programs (two shaders of the port's file of SPIR-V, tools/spirv.py). OpenGL's state is kept as it is set; a draw makes
// Vulkan's own of it: a pipeline for the program and the kinds of the targets (kept, once made), the rest said as it is drawn.
// Commands go into one command buffer a picture, handed over when the picture is shown; three pictures' worth are kept in turn.
#ifdef __SWITCH2__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define VK_USE_PLATFORM_VI_NN
#include <GLES3/gl32.h>
#include <vulkan/vulkan.h>

#include "nx2.h"

int nx2_vulkan;

#define SAY(limit, ...) \
    do { \
        static u32 said_; \
        if (said_ < (limit)) said_++, nx2_log(__VA_ARGS__); \
    } while (0)

// ---------------------------------------------------------------- the device
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage chain_images[8];
static u32 chain_count;
static VkExtent2D chain_extent;
static VkFormat chain_format;
static VkCommandPool pool;
static PFN_vkCmdPushDescriptorSetKHR push_descriptors;
static PFN_vkCmdSetVertexInputEXT set_vertex_input;
static PFN_vkCmdSetColorBlendEnableEXT set_blend_enable;
static PFN_vkCmdSetColorBlendEquationEXT set_blend_equation;
static PFN_vkCmdSetColorWriteMaskEXT set_write_mask;
static int custom_borders;

#define FRAMES 3
#define STAGE_ROOM (96u << 20)
typedef struct {
    int kind;  // 1 a buffer and its memory, 2 an image and its memory, 3 an image view, 4 a sampler
    u64 a, b;
} Retired;
static struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, drawn;
    VkBuffer stage;
    VkDeviceMemory stage_memory;
    u8 *stage_at;
    Retired *retired;
    u32 nretired, retired_room;
} frames[FRAMES];
static u32 frame;         // which of them is being recorded
static u32 serial = 1;    // how many pictures have been begun
static VkCommandBuffer cmd;
static u32 stage_used;
static int rendering;     // a pass of drawing is open, to the framebuffer rendering_fbo
static u32 rendering_fbo, rendering_w, rendering_h;

static void failed(const char *what, VkResult r) { SAY(60, "[vk] %s: result %d\n", what, (s64)r); }
#define CHECK(what, call) \
    do { \
        const VkResult r_ = (call); \
        if (r_ < 0) failed(what, r_); \
    } while (0)

static u32 memory_type(u32 allowed, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(physical, &m);
    for (u32 k = 0; k < m.memoryTypeCount; k++)
        if ((allowed >> k & 1) && (m.memoryTypes[k].propertyFlags & wanted) == wanted) return k;
    return 0;
}
static void retire(int kind, u64 a, u64 b) {
    __typeof__(frames[0]) *f = &frames[frame];
    if (f->nretired == f->retired_room) {
        f->retired_room = f->retired_room ? f->retired_room * 2 : 256;
        f->retired = realloc(f->retired, f->retired_room * sizeof *f->retired);
    }
    f->retired[f->nretired++] = (Retired){kind, a, b};
}
static void retired_free(u32 k) {
    __typeof__(frames[0]) *f = &frames[k];
    for (u32 i = 0; i < f->nretired; i++) {
        const Retired *r = &f->retired[i];
        if (r->kind == 1) vkDestroyBuffer(device, (VkBuffer)r->a, NULL), vkFreeMemory(device, (VkDeviceMemory)r->b, NULL);
        else if (r->kind == 2) vkDestroyImage(device, (VkImage)r->a, NULL), vkFreeMemory(device, (VkDeviceMemory)r->b, NULL);
        else if (r->kind == 3) vkDestroyImageView(device, (VkImageView)r->a, NULL);
        else if (r->kind == 4) vkDestroySampler(device, (VkSampler)r->a, NULL);
    }
    f->nretired = 0;
}
static void rendering_end(void) {
    if (rendering) vkCmdEndRendering(cmd), rendering = 0;
}
static void commands_begin(void) {
    cmd = frames[frame].cmd;
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    stage_used = 0;
}
static void state_all_new(void);
// what has been recorded is handed to the GPU now and waited for: when the room for texels on their way is full, or texels are read back
static void commands_flush(void) {
    rendering_end();
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    vkResetFences(device, 1, &frames[frame].fence);
    CHECK("a submit in the middle of a picture", vkQueueSubmit(queue, 1, &si, frames[frame].fence));
    vkWaitForFences(device, 1, &frames[frame].fence, VK_TRUE, ~0ull);
    commands_begin();
    state_all_new();
}
// room for so many bytes on their way to an image: where they are to be written, and where that is in the buffer
static u8 *stage_take(u64 bytes, VkDeviceSize *at) {
    bytes = (bytes + 15) & ~15ull;
    if (bytes > STAGE_ROOM) return NULL;
    if (stage_used + bytes > STAGE_ROOM) commands_flush();
    *at = stage_used;
    stage_used += (u32)bytes;
    return frames[frame].stage_at + *at;
}

// ---------------------------------------------------------------- formats
typedef struct {
    u32 gl;
    VkFormat vk;
    u8 bytes, bw, bh, depth;  // a texel's bytes (a block's, compressed, bw by bh texels); depth: 1 depth, 2 depth and stencil
} Kind;
static const Kind kinds[] = {
    {GL_R8, VK_FORMAT_R8_UNORM, 1, 1, 1, 0}, {GL_R8_SNORM, VK_FORMAT_R8_SNORM, 1, 1, 1, 0}, {GL_R16F, VK_FORMAT_R16_SFLOAT, 2, 1, 1, 0},
    {GL_R32F, VK_FORMAT_R32_SFLOAT, 4, 1, 1, 0}, {GL_RG8, VK_FORMAT_R8G8_UNORM, 2, 1, 1, 0}, {GL_RG8_SNORM, VK_FORMAT_R8G8_SNORM, 2, 1, 1, 0},
    {GL_RG16F, VK_FORMAT_R16G16_SFLOAT, 4, 1, 1, 0}, {GL_RG32F, VK_FORMAT_R32G32_SFLOAT, 8, 1, 1, 0},
    {GL_RGB8, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 0},  // (three bytes a texel as OpenGL is given them: made four on their way)
    {GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 0}, {GL_RGBA8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4, 1, 1, 0}, {GL_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, 1, 0},
    {GL_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, 1, 0}, {GL_SRGB8_ALPHA8, VK_FORMAT_R8G8B8A8_SRGB, 4, 1, 1, 0},
    {GL_RGBA4, VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, 1, 1, 0}, {GL_RGB5_A1, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2, 1, 1, 0},
    {GL_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 1, 0}, {GL_RGB10_A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, 1, 0},
    {GL_R11F_G11F_B10F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1, 1, 0}, {GL_RGBA32UI, VK_FORMAT_R32G32B32A32_UINT, 16, 1, 1, 0},
    {GL_RG32UI, VK_FORMAT_R32G32_UINT, 8, 1, 1, 0}, {GL_RGBA16UI, VK_FORMAT_R16G16B16A16_UINT, 8, 1, 1, 0},
    {GL_DEPTH_COMPONENT16, VK_FORMAT_D16_UNORM, 2, 1, 1, 1}, {GL_DEPTH_COMPONENT24, VK_FORMAT_X8_D24_UNORM_PACK32, 4, 1, 1, 1},
    {GL_DEPTH_COMPONENT32F, VK_FORMAT_D32_SFLOAT, 4, 1, 1, 1}, {GL_DEPTH24_STENCIL8, VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 2},
    {GL_DEPTH32F_STENCIL8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, 1, 2},
    {0x83F0, VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, 4, 4, 0}, {0x83F1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4, 0}, {0x83F2, VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4, 0},
    {0x83F3, VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4, 0}, {0x8C4C, VK_FORMAT_BC1_RGB_SRGB_BLOCK, 8, 4, 4, 0}, {0x8C4D, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4, 4, 0},
    {0x8C4E, VK_FORMAT_BC2_SRGB_BLOCK, 16, 4, 4, 0}, {0x8C4F, VK_FORMAT_BC3_SRGB_BLOCK, 16, 4, 4, 0}, {0x8DBB, VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, 4, 0},
    {0x8DBC, VK_FORMAT_BC4_SNORM_BLOCK, 8, 4, 4, 0}, {0x8DBD, VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, 4, 0}, {0x8DBE, VK_FORMAT_BC5_SNORM_BLOCK, 16, 4, 4, 0},
    {0x8E8C, VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, 4, 0}, {0x8E8D, VK_FORMAT_BC7_SRGB_BLOCK, 16, 4, 4, 0}, {0x8E8E, VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4, 4, 0},
    {0x8E8F, VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4, 4, 0},
};
// (ASTC: OpenGL's numbers 0x93B0 to 0x93BD, and their sRGB kind from 0x93D0, are Vulkan's 157 on, two by two, in the same order)
static const u8 astc_size[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static int kind_of(u32 gl, Kind *out) {
    for (u32 k = 0; k < sizeof kinds / sizeof *kinds; k++)
        if (kinds[k].gl == gl) {
            *out = kinds[k];
            return 1;
        }
    if ((gl >= 0x93B0 && gl <= 0x93BD) || (gl >= 0x93D0 && gl <= 0x93DD)) {
        const u32 n = gl & 15, srgb = gl >= 0x93D0;
        *out = (Kind){gl, (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * n + srgb), 16, astc_size[n][0], astc_size[n][1], 0};
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- textures
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkFormat format;
    u32 w, h, d, levels, layers, refs;
    int is3d, depth, attachable;
    VkImageLayout layout;
} Image;
#ifdef NX2_GAME_MB  // (a Switch 2 game draws with its own SDK: none of this is ever used; nvn2.c's ARENA has the story)
#define TEXS 16
#else
#define TEXS 65536
#endif
typedef struct {
    int used;
    Image *im;
    u32 target;              // OpenGL's kind
    Kind kind;               // what it is seen as (a view may see another format of the same size)
    u32 level0, levels, layer0, layers;
    u32 swizzle[4];          // OpenGL's names: GL_RED..., GL_ZERO, GL_ONE
    u32 min, mag, ws, wt, wr;
    VkImageView view;        // for reading (made when first read, and again when what it shows has changed)
    VkSampler sampler;       // its own way of being read, for a draw that binds no sampler
    struct {
        u32 where;           // level | layer << 8, + 1
        VkImageView view;
    } targets[6];
} Tex;
static Tex texs[TEXS];
static u32 tex_next = 1;

#define SAMPS 1024
typedef struct {
    int used;
    u32 min, mag, ws, wt, wr, compare, func;
    float lod_min, lod_max, border[4];
    VkSampler vk;
} Samp;
static Samp samps[SAMPS];

// ---------------------------------------------------------------- buffers
// Memory for OpenGL's buffers: blocks of 32 MB, each one buffer of Vulkan's that stays mapped, cut into pieces of sizes that are
// powers of two (from 256 bytes); a piece given back is the next of its size to be given. A buffer larger than 8 MB has a
// block of its own.
#define BLOCK (32u << 20)
#define BLOCKS 96
static struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    u8 *at;
    u32 used, size;
} blocks[BLOCKS];
static u32 nblocks;
typedef struct {
    u16 block;
    u8 size;  // log2 (0: none)
    u32 at;
} Piece;
static Piece piece_free[32][256];
static u32 piece_free_count[32];
static const VkBufferUsageFlags every_use = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
static int block_make(u32 size) {
    if (nblocks == BLOCKS) return -1;
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = every_use};
    if (vkCreateBuffer(device, &bci, NULL, &blocks[nblocks].buffer) < 0) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, blocks[nblocks].buffer, &req);
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                      .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void *at = NULL;
    if (vkAllocateMemory(device, &mai, NULL, &blocks[nblocks].memory) < 0 || vkBindBufferMemory(device, blocks[nblocks].buffer, blocks[nblocks].memory, 0) < 0 ||
        vkMapMemory(device, blocks[nblocks].memory, 0, size, 0, &at) < 0) {
        SAY(8, "[vk] no memory for another block of buffers (%u bytes; %u blocks so far)\n", (u64)size, (u64)nblocks);
        return -1;
    }
    blocks[nblocks].at = at, blocks[nblocks].size = size, blocks[nblocks].used = 0;
    return (int)nblocks++;
}
static Piece piece_take(u64 bytes) {
    u8 size = 8;
    while ((1ull << size) < bytes) size++;
    if (piece_free_count[size]) return piece_free[size][--piece_free_count[size]];
    if (size > 23) {  // (a block of its own)
        const int b = block_make(1u << size);
        if (b < 0) return (Piece){0, 0, 0};
        blocks[b].used = blocks[b].size;
        return (Piece){(u16)b, size, 0};
    }
    for (u32 b = 0; b <= nblocks; b++) {
        if (b == nblocks && block_make(BLOCK) < 0) break;
        if (blocks[b].size == BLOCK && blocks[b].used + (1u << size) <= BLOCK) {
            const u32 at = (blocks[b].used + (1u << size) - 1) & ~((1u << size) - 1);  // (a piece lies at a multiple of its size)
            if (at + (1u << size) > BLOCK) continue;
            blocks[b].used = at + (1u << size);
            return (Piece){(u16)b, size, at};
        }
    }
    return (Piece){0, 0, 0};
}
// (a piece given back may still be read by the GPU for the pictures on their way: it waits three pictures before it is given again)
static struct {
    Piece piece;
    u32 serial;
} piece_waiting[4096];
static u32 piece_waiting_count;
static void piece_give(Piece p) {
    if (!p.size) return;
    if (piece_waiting_count < 4096) piece_waiting[piece_waiting_count].piece = p, piece_waiting[piece_waiting_count++].serial = serial;
}
static void pieces_settle(void) {
    u32 kept = 0;
    for (u32 k = 0; k < piece_waiting_count; k++) {
        const Piece p = piece_waiting[k].piece;
        if (serial - piece_waiting[k].serial >= FRAMES && piece_free_count[p.size] < 256) piece_free[p.size][piece_free_count[p.size]++] = p;
        else piece_waiting[kept++] = piece_waiting[k];
    }
    piece_waiting_count = kept;
}
#define BUFS 16384
typedef struct {
    int used;
    Piece piece;
    u32 size;        // what OpenGL was told it holds
    u32 read_top;    // how far into it draws recorded since it was last given anew may read
    u32 read_serial; // at which picture a draw last read it
} Buf;
static Buf bufs[BUFS];
static u32 buf_next = 1;
static u8 *buf_at(const Buf *b) { return blocks[b->piece.block].at + b->piece.at; }

// ---------------------------------------------------------------- framebuffers, programs, state
#define FBS 4096
typedef struct {
    int used;
    struct {
        u32 tex, level, layer;
    } color[8], depth;
    int stencil;  // the depth's texture is attached as stencil too
    u32 draws;    // which of the colors are drawn to (a bit each)
} Fb;
static Fb fbs[FBS];
static u32 fb_next = 1;
static u32 window_tex;  // the window, as a texture: framebuffer 0 draws into it, and it is copied to the screen when a picture is shown

typedef struct {
    u64 key;
    u32 stage;
    VkShaderModule module;
    u32 blocks[16];
    u8 textures[32];
    u32 storage, own, between;
} Shader;
#define PROGS 16384
typedef struct {
    int used;
    Shader *vs, *fs;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    u8 unit_binding[32];  // the binding of Vulkan's that each of OpenGL's texture units stands for
    u8 unit_kind[32];
    u32 units;
} Prog;
static Prog progs[PROGS];
static u32 prog_next = 1;

static struct {
    int x, y, w, h;
} viewport, scissor;
static struct {
    u32 fb_draw, fb_read, program, active, array_buffer, element_buffer, uniform_buffer, storage_buffer;
    u32 unit_tex[32], unit_samp[32];
    struct {
        u32 buffer, offset, size;
    } ubo[32], ssbo[16];
    struct {
        int on, parts, whole, norm;
        u32 type, stride, buffer, divisor;
        u64 offset;
    } attrib[16];
    int scissor_on, depth_test, depth_write, stencil_test, cull_on, offset_on, flip, depth_zero;
    u32 depth_func, cull_face, front_face;
    float near, far, offset_factor, offset_units, blend_color[4];
    struct {
        u32 on, src, dst, srca, dsta, eq, eqa, mask;
    } blend[8];
    struct {
        u32 func, ref, value_mask, write_mask, fail, zfail, zpass;
    } stencil[2];
    int row_length;
    int fresh;  // the command buffer has been begun anew: everything is to be said again at the next draw
} gl;
static void state_all_new(void) { gl.fresh = 1; }

static u32 texture_index(u32 id) { return id < TEXS && texs[id].used ? id : 0; }

// ---------------------------------------------------------------- starting
static FILE *spv_file;
static struct {
    u64 key;
    u32 stage, size;
    u64 at;
    u32 blocks[16];
    u8 textures[32];
    u32 storage, own, between, pad;
} *spv_rows;
static u32 spv_count;
static Shader *shaders;


static void pipelines_at_start(const char *pack);
void glvk_start(int w, int h, const char *pack) {
    const char *instance_extensions[] = {"VK_KHR_surface", "VK_NN_vi_surface"};
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nx2", .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2,
                                      .ppEnabledExtensionNames = instance_extensions};
    CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &instance));
    u32 count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    nwindowSetDimensions(nwindowGetDefault(), (u32)w, (u32)h);
    const VkViSurfaceCreateInfoNN sci = {.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = nwindowGetDefault()};
    CHECK("vkCreateViSurfaceNN", vkCreateViSurfaceNN(instance, &sci, NULL, &surface));

    VkPhysicalDeviceCustomBorderColorFeaturesEXT borders = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .pNext = &borders};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamic2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .pNext = &dynamic3};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .pNext = &dynamic2};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT index8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, .pNext = &vertex_input};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &index8};
    VkPhysicalDeviceFeatures2 has = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(physical, &has);
    nx2_log("[vk] the device has: dynamic rendering %u, vertex input said at each draw %u, blend said at each draw %u %u %u, indices of one byte %u, borders of any color %u, "
            "robust buffers %u, BC %u, ASTC %u\n",
            (u64)v13.dynamicRendering, (u64)vertex_input.vertexInputDynamicState, (u64)dynamic3.extendedDynamicState3ColorBlendEnable,
            (u64)dynamic3.extendedDynamicState3ColorBlendEquation, (u64)dynamic3.extendedDynamicState3ColorWriteMask, (u64)index8.indexTypeUint8,
            (u64)borders.customBorderColors, (u64)has.features.robustBufferAccess, (u64)has.features.textureCompressionBC, (u64)has.features.textureCompressionASTC_LDR);
    custom_borders = borders.customBorderColors;
    {  // (only what is used is asked for)
        const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT d3 = dynamic3;
        memset((u8 *)&dynamic3 + offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin), 0,
               sizeof dynamic3 - offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin));
        dynamic3.extendedDynamicState3ColorBlendEnable = d3.extendedDynamicState3ColorBlendEnable;
        dynamic3.extendedDynamicState3ColorBlendEquation = d3.extendedDynamicState3ColorBlendEquation;
        dynamic3.extendedDynamicState3ColorWriteMask = d3.extendedDynamicState3ColorWriteMask;
        const VkPhysicalDeviceFeatures f = has.features;
        memset(&has.features, 0, sizeof has.features);
        has.features.robustBufferAccess = f.robustBufferAccess, has.features.textureCompressionBC = f.textureCompressionBC;
        has.features.textureCompressionASTC_LDR = f.textureCompressionASTC_LDR, has.features.independentBlend = f.independentBlend;
        has.features.imageCubeArray = f.imageCubeArray, has.features.samplerAnisotropy = f.samplerAnisotropy, has.features.depthClamp = f.depthClamp;
        has.features.depthBiasClamp = f.depthBiasClamp, has.features.fragmentStoresAndAtomics = f.fragmentStoresAndAtomics;
        const VkBool32 rendering_ = v13.dynamicRendering;
        memset((u8 *)&v13 + offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess), 0, sizeof v13 - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
        v13.dynamicRendering = rendering_;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {"VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_EXT_vertex_input_dynamic_state", "VK_EXT_extended_dynamic_state2",
                                       "VK_EXT_extended_dynamic_state3", "VK_EXT_index_type_uint8", "VK_EXT_custom_border_color"};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &has, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = sizeof device_extensions / sizeof *device_extensions, .ppEnabledExtensionNames = device_extensions};
    CHECK("vkCreateDevice", vkCreateDevice(physical, &dci, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    push_descriptors = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(device, "vkCmdSetVertexInputEXT");
    set_blend_enable = (PFN_vkCmdSetColorBlendEnableEXT)vkGetDeviceProcAddr(device, "vkCmdSetColorBlendEnableEXT");
    set_blend_equation = (PFN_vkCmdSetColorBlendEquationEXT)vkGetDeviceProcAddr(device, "vkCmdSetColorBlendEquationEXT");
    set_write_mask = (PFN_vkCmdSetColorWriteMaskEXT)vkGetDeviceProcAddr(device, "vkCmdSetColorWriteMaskEXT");
    if (!push_descriptors || !set_vertex_input || !set_blend_enable || !set_blend_equation || !set_write_mask)
        nx2_log("[vk] the driver LACKS a function the port draws with (%p %p %p %p %p)\n", (u64)push_descriptors, (u64)set_vertex_input, (u64)set_blend_enable,
                (u64)set_blend_equation, (u64)set_write_mask);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    VkSurfaceFormatKHR formats[8];
    u32 nformats = 8;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nformats, formats);
    chain_extent = caps.currentExtent, chain_format = formats[0].format;
    const VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = 3, .imageFormat = chain_format,
                                           .imageColorSpace = formats[0].colorSpace, .imageExtent = chain_extent, .imageArrayLayers = 1,
                                           .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                           .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    CHECK("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
    chain_count = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &chain_count, chain_images);
    nx2_log("[vk] the screen: %ux%u, format %d, %u pictures in turn; the game's window is %dx%d\n", (u64)chain_extent.width, (u64)chain_extent.height, (s64)chain_format,
            (u64)chain_count, (s64)w, (s64)h);

    const VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CHECK("vkCreateCommandPool", vkCreateCommandPool(device, &pci, NULL, &pool));
    for (u32 k = 0; k < FRAMES; k++) {
        const VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        vkAllocateCommandBuffers(device, &cai, &frames[k].cmd);
        const VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        vkCreateSemaphore(device, &sem, NULL, &frames[k].acquired);
        vkCreateSemaphore(device, &sem, NULL, &frames[k].drawn);
        vkCreateFence(device, &fci, NULL, &frames[k].fence);
        const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = STAGE_ROOM, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK("a buffer for texels on their way", vkCreateBuffer(device, &bci, NULL, &frames[k].stage));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, frames[k].stage, &req);
        const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                          .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        void *at = NULL;
        CHECK("its memory", vkAllocateMemory(device, &mai, NULL, &frames[k].stage_memory));
        vkBindBufferMemory(device, frames[k].stage, frames[k].stage_memory, 0);
        vkMapMemory(device, frames[k].stage_memory, 0, STAGE_ROOM, 0, &at);
        frames[k].stage_at = at;
    }
    // the port's file of SPIR-V: its rows are read now, a shader's code when a program first asks for it
    spv_file = fopen(pack, "rb");
    u32 head[4] = {0};
    if (!spv_file || fread(head, 4, 4, spv_file) != 4 || head[0] != 0x5632584E /* NX2V */ || head[1] != 1 || head[3] != sizeof *spv_rows) {
        nx2_log("[vk] the shaders as SPIR-V (%s): %s\n", pack, spv_file ? "not a file of this port's" : "not there");
        if (spv_file) fclose(spv_file), spv_file = NULL;
    } else {
        spv_rows = malloc((size_t)head[2] * sizeof *spv_rows);
        if (spv_rows && fread(spv_rows, sizeof *spv_rows, head[2], spv_file) == head[2]) spv_count = head[2];
        shaders = calloc(spv_count ? spv_count : 1, sizeof *shaders);
        nx2_log("[vk] the shaders as SPIR-V (%s): %u of them\n", pack, (u64)spv_count);
    }
    // the window: a texture of the port's, which framebuffer 0 draws into
    frame = 0;
    vkResetFences(device, 1, &frames[0].fence);
    commands_begin();
    gl.near = 0, gl.far = 1, gl.front_face = GL_CCW, gl.cull_face = GL_BACK, gl.depth_func = GL_LESS, gl.depth_write = 1;
    for (int k = 0; k < 8; k++) gl.blend[k].src = gl.blend[k].srca = GL_ONE, gl.blend[k].dst = gl.blend[k].dsta = GL_ZERO, gl.blend[k].eq = gl.blend[k].eqa = GL_FUNC_ADD, gl.blend[k].mask = 15;
    for (int k = 0; k < 2; k++) gl.stencil[k].func = GL_ALWAYS, gl.stencil[k].value_mask = gl.stencil[k].write_mask = 255, gl.stencil[k].fail = gl.stencil[k].zfail = gl.stencil[k].zpass = GL_KEEP;
    viewport.w = scissor.w = w, viewport.h = scissor.h = h;
    extern void vk_glGenTextures(GLsizei, GLuint *);
    extern void vk_glBindTexture(GLenum, GLuint);
    extern void vk_glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    vk_glGenTextures(1, &window_tex);
    vk_glBindTexture(GL_TEXTURE_2D, window_tex);
    vk_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    vk_glBindTexture(GL_TEXTURE_2D, 0);
    state_all_new();
    pipelines_at_start(pack);
    nx2_vulkan = 1;
}

// ---------------------------------------------------------------- images and their views
static void image_layout(Image *im, VkImageLayout to) {
    if (im->layout == to) return;
    rendering_end();
    const VkImageAspectFlags aspect = im->depth == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : im->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    .oldLayout = im->layout, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = im->image, .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}
static VkImageAspectFlags aspect_of(const Tex *t, int reading) {
    if (!t->kind.depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return t->kind.depth == 2 && !reading ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}
static void tex_views_gone(Tex *t) {
    if (t->view) retire(3, (u64)t->view, 0), t->view = VK_NULL_HANDLE;
    for (int k = 0; k < 6; k++)
        if (t->targets[k].where) retire(3, (u64)t->targets[k].view, 0), t->targets[k].where = 0;
}
static void tex_gone(Tex *t) {
    tex_views_gone(t);
    if (t->sampler) retire(4, (u64)t->sampler, 0), t->sampler = VK_NULL_HANDLE;
    if (t->im && --t->im->refs == 0) {
        retire(2, (u64)t->im->image, (u64)t->im->memory);
        free(t->im);
    }
    memset(t, 0, sizeof *t);
}
static VkComponentSwizzle swizzle_of(u32 gl_name, VkComponentSwizzle same) {
    (void)same;
    return gl_name == GL_RED ? VK_COMPONENT_SWIZZLE_R : gl_name == GL_GREEN ? VK_COMPONENT_SWIZZLE_G : gl_name == GL_BLUE ? VK_COMPONENT_SWIZZLE_B :
           gl_name == GL_ALPHA ? VK_COMPONENT_SWIZZLE_A : gl_name == GL_ZERO ? VK_COMPONENT_SWIZZLE_ZERO : gl_name == GL_ONE ? VK_COMPONENT_SWIZZLE_ONE : same;
}
static VkImageView tex_view(Tex *t) {
    if (t->view || !t->im) return t->view;
    const u32 kind = t->target;
    const VkImageViewType type = t->im->is3d ? VK_IMAGE_VIEW_TYPE_3D : kind == GL_TEXTURE_CUBE_MAP ? VK_IMAGE_VIEW_TYPE_CUBE : kind == GL_TEXTURE_CUBE_MAP_ARRAY ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
                                 kind == GL_TEXTURE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    u32 layers = type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_3D ? 1 : t->layers;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
    const VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->im->image, .viewType = type, .format = t->kind.vk,
                                       .components = {swizzle_of(t->swizzle[0], VK_COMPONENT_SWIZZLE_R), swizzle_of(t->swizzle[1], VK_COMPONENT_SWIZZLE_G),
                                                      swizzle_of(t->swizzle[2], VK_COMPONENT_SWIZZLE_B), swizzle_of(t->swizzle[3], VK_COMPONENT_SWIZZLE_A)},
                                       .subresourceRange = {aspect_of(t, 1), t->level0, t->levels, t->im->is3d ? 0 : t->layer0, layers}};
    const VkResult r = vkCreateImageView(device, &vci, NULL, &t->view);
    if (r < 0) SAY(20, "[vk] a view of a texture (kind %x, format %d, %u levels from %u, %u layers from %u of %u) isn't made: result %d\n", (u64)kind, (s64)t->kind.vk, (u64)t->levels,
                   (u64)t->level0, (u64)layers, (u64)t->layer0, (u64)t->im->layers, (s64)r);
    return t->view;
}
// one level of one layer (a 3D texture's slice), as a target
static VkImageView tex_target(Tex *t, u32 level, u32 layer) {
    const u32 where = (level | layer << 8) + 1;
    int at = 0;
    for (int k = 0; k < 6; k++) {
        if (t->targets[k].where == where) return t->targets[k].view;
        if (!t->targets[k].where) at = k;
    }
    if (t->targets[at].where) retire(3, (u64)t->targets[at].view, 0);
    const VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->im->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = t->kind.vk,
                                       .subresourceRange = {aspect_of(t, 0), t->level0 + level, 1, t->layer0 + layer, 1}};
    t->targets[at].view = VK_NULL_HANDLE;
    const VkResult r = vkCreateImageView(device, &vci, NULL, &t->targets[at].view);
    if (r < 0) SAY(20, "[vk] a texture's level %u, layer %u isn't to be had as a target (format %d): result %d\n", (u64)level, (u64)layer, (s64)t->kind.vk, (s64)r);
    t->targets[at].where = where;
    return t->targets[at].view;
}
static void storage(u32 target, int levels, u32 internal, int w, int h, int d) {
    Tex *t = &texs[gl.unit_tex[gl.active]];
    Kind kind;
    if (!t->used || t->im) return;
    if (!kind_of(internal, &kind)) {
        SAY(20, "[vk] a texture of OpenGL's format %x, which isn't known here: taken for RGBA8\n", (u64)internal);
        kind_of(GL_RGBA8, &kind);
    }
    Image *im = calloc(1, sizeof *im);
    const int is3d = target == GL_TEXTURE_3D, cube = target == GL_TEXTURE_CUBE_MAP;
    const u32 layers = is3d ? 1 : cube ? 6 : (u32)(d > 0 ? d : 1);
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical, kind.vk, &props);
    const int attachable = (props.optimalTilingFeatures & (kind.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) != 0;
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) SAY(20, "[vk] the device can't read textures of format %d (OpenGL's %x)\n", (s64)kind.vk, (u64)internal);
    const VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                   .flags = (kind.depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) | (!is3d && layers >= 6 && w == h ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                            (is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) | (kind.bw > 1 ? VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT : 0),
                                   .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = kind.vk, .extent = {(u32)w, (u32)h, is3d ? (u32)d : 1},
                                   .mipLevels = (u32)levels, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            (attachable ? (kind.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0)};
    VkResult r = vkCreateImage(device, &ici, NULL, &im->image);
    if (r >= 0) {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, im->image, &req);
        const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                          .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        r = vkAllocateMemory(device, &mai, NULL, &im->memory);
        if (r >= 0) r = vkBindImageMemory(device, im->image, im->memory, 0);
    }
    if (r < 0) {
        SAY(40, "[vk] a texture of %dx%dx%d, %d levels, format %d (OpenGL's %x), kind %x isn't made: result %d\n", (s64)w, (s64)h, (s64)d, (s64)levels, (s64)kind.vk, (u64)internal,
            (u64)target, (s64)r);
        free(im);
        return;
    }
    im->format = kind.vk, im->w = (u32)w, im->h = (u32)h, im->d = is3d ? (u32)d : 1, im->levels = (u32)levels, im->layers = layers, im->refs = 1, im->is3d = is3d;
    im->depth = kind.depth, im->attachable = attachable, im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->im = im, t->target = target, t->kind = kind, t->level0 = 0, t->levels = (u32)levels, t->layer0 = 0, t->layers = layers;
}

// texels on their way into a texture: in plain rows (bytes 0: by its format), or in compressed blocks
static void texels_give(u32 target, int level, int x, int y, int z, int w, int h, int d, u32 format, u32 type, const void *texels, u32 bytes) {
    Tex *t = &texs[gl.unit_tex[gl.active]];
    if (!t->used || !t->im || !texels) return;
    u32 layer = 0;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6) layer = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
    else if (!t->im->is3d) layer = (u32)z;
    const u32 layers = t->im->is3d || target != t->target || t->target == GL_TEXTURE_2D ? 1 : (u32)d;
    {
        // (Texels that don't lie within the level aren't given: OpenGL refuses them with an error, which nvn2.c has been met with
        // for a texture or two of a game's at every start; Vulkan, told to copy them, loses the device.)
        const u32 l = t->level0 + (u32)level;
        const u32 lw = l < t->im->levels ? (t->im->w >> l ? t->im->w >> l : 1) : 0, lh = l < t->im->levels ? (t->im->h >> l ? t->im->h >> l : 1) : 0;
        const u32 lw_ = (lw + t->kind.bw - 1) / t->kind.bw * t->kind.bw, lh_ = (lh + t->kind.bh - 1) / t->kind.bh * t->kind.bh;
        const u32 ld = t->im->is3d ? (t->im->d >> l ? t->im->d >> l : 1) : 1;
        // (a compressed level is given in whole blocks or to its own edge)
        const int fits = lw && w > 0 && h > 0 && x >= 0 && y >= 0 && (u32)x + (u32)w <= lw_ && (u32)y + (u32)h <= lh_ &&
                         ((u32)x + (u32)w <= lw || (u32)w % t->kind.bw == 0 || (u32)x + (u32)w == lw) && (t->im->is3d ? z >= 0 && (u32)z + (u32)d <= ld : t->layer0 + layer + layers <= t->im->layers);
        u32 gw = (u32)w, gh = (u32)h;
        if (fits && (u32)x + gw > lw) gw = lw - (u32)x;  // (the last block of a row, past the level's edge: Vulkan takes the texels up to the edge)
        if (fits && (u32)y + gh > lh) gh = lh - (u32)y;
        if (!fits) {
            SAY(12, "[vk] texels that don't lie within their level aren't given (%dx%dx%d at %d,%d,%d of level %d, which is %ux%u; format %d, %u layers of %u)\n", (s64)w, (s64)h, (s64)d,
                (s64)x, (s64)y, (s64)z, (s64)level, (u64)lw, (u64)lh, (s64)t->kind.vk, (u64)layers, (u64)t->im->layers);
            return;
        }
        w = (int)gw, h = (int)gh;
    }
    const int row_texels = !bytes && gl.row_length > 0 ? gl.row_length : w;
    const int three = !bytes && format == GL_RGB && type == GL_UNSIGNED_BYTE;
    u64 need = bytes;
    if (!bytes) need = (u64)row_texels * (u64)h * (u64)(t->im->is3d ? d : (int)layers) * (three ? 4 : t->kind.bytes);
    VkDeviceSize at = 0;
    u8 *to = stage_take(need, &at);
    if (!to) {
        SAY(8, "[vk] %u bytes of texels are more than can be on their way at once: left out\n", need);
        return;
    }
    if (three) {
        const u8 *from = texels;
        for (u64 k = 0; k < need / 4; k++) to[4 * k] = from[3 * k], to[4 * k + 1] = from[3 * k + 1], to[4 * k + 2] = from[3 * k + 2], to[4 * k + 3] = 255;
    } else {
        memcpy(to, texels, need);
    }
    image_layout(t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy copy = {.bufferOffset = at, .bufferRowLength = bytes ? 0 : (u32)row_texels,
                                    .imageSubresource = {t->kind.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, t->level0 + (u32)level, t->layer0 + layer, layers},
                                    .imageOffset = {x, y, t->im->is3d ? z : 0}, .imageExtent = {(u32)w, (u32)h, t->im->is3d ? (u32)d : 1}};
    vkCmdCopyBufferToImage(cmd, frames[frame].stage, t->im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// ---------------------------------------------------------------- OpenGL's functions: textures
void vk_glGenTextures(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) {
        u32 id = 0;
        for (u32 tries = 0; tries < TEXS; tries++, tex_next = tex_next + 1 < TEXS ? tex_next + 1 : 1)
            if (!texs[tex_next].used) {
                id = tex_next;
                break;
            }
        if (id) {
            memset(&texs[id], 0, sizeof texs[id]);
            texs[id].used = 1, texs[id].swizzle[0] = GL_RED, texs[id].swizzle[1] = GL_GREEN, texs[id].swizzle[2] = GL_BLUE, texs[id].swizzle[3] = GL_ALPHA;
            texs[id].min = GL_NEAREST_MIPMAP_LINEAR, texs[id].mag = GL_LINEAR, texs[id].ws = texs[id].wt = texs[id].wr = GL_REPEAT;
        }
        out[k] = id;
    }
}
void vk_glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++) {
        const u32 id = ids[k];
        if (!texture_index(id) || id == window_tex) continue;
        for (int u = 0; u < 32; u++)
            if (gl.unit_tex[u] == id) gl.unit_tex[u] = 0;
        for (u32 f = 1; f < FBS; f++) {
            if (!fbs[f].used) continue;
            for (int c = 0; c < 8; c++)
                if (fbs[f].color[c].tex == id) fbs[f].color[c].tex = 0;
            if (fbs[f].depth.tex == id) fbs[f].depth.tex = 0;
        }
        if (rendering) rendering_end();
        tex_gone(&texs[id]);
    }
}
void vk_glActiveTexture(GLenum unit) { gl.active = (unit - GL_TEXTURE0) & 31; }
void vk_glBindTexture(GLenum target, GLuint id) {
    (void)target;
    gl.unit_tex[gl.active] = texture_index(id);
}
void vk_glTexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h) { storage(target, levels, internal, w, h, 1); }
void vk_glTexStorage3D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d) { storage(target, levels, internal, w, h, d); }
void vk_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *texels) {
    texels_give(target, level, x, y, 0, w, h, 1, format, type, texels, 0);
}
void vk_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *texels) {
    texels_give(target, level, x, y, z, w, h, d, format, type, texels, 0);
}
void vk_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei bytes, const void *data) {
    texels_give(target, level, x, y, 0, w, h, 1, format, 0, data, (u32)bytes);
}
void vk_glCompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLsizei bytes, const void *data) {
    texels_give(target, level, x, y, z, w, h, d, format, 0, data, (u32)bytes);
}
void vk_glTexParameteri(GLenum target, GLenum name, GLint value) {
    (void)target;
    Tex *t = &texs[gl.unit_tex[gl.active]];
    if (!t->used) return;
    const u32 v = (u32)value;
    if (name >= GL_TEXTURE_SWIZZLE_R && name <= GL_TEXTURE_SWIZZLE_A) {
        if (t->swizzle[name - GL_TEXTURE_SWIZZLE_R] != v) t->swizzle[name - GL_TEXTURE_SWIZZLE_R] = v, tex_views_gone(t);
        return;
    }
    u32 *what = name == GL_TEXTURE_MIN_FILTER ? &t->min : name == GL_TEXTURE_MAG_FILTER ? &t->mag : name == GL_TEXTURE_WRAP_S ? &t->ws : name == GL_TEXTURE_WRAP_T ? &t->wt :
                name == GL_TEXTURE_WRAP_R ? &t->wr : NULL;
    if (what && *what != v) {
        *what = v;
        if (t->sampler) retire(4, (u64)t->sampler, 0), t->sampler = VK_NULL_HANDLE;
    }
}
void vk_glPixelStorei(GLenum name, GLint value) {
    if (name == GL_UNPACK_ROW_LENGTH) gl.row_length = value;
}
// glTextureViewOES: a texture that is another's texels, seen as another kind or format
void vk_glTextureView(GLuint id, GLenum target, GLuint of, GLenum internal, GLuint level0, GLuint levels, GLuint layer0, GLuint layers) {
    Tex *t = &texs[texture_index(id)], *from = &texs[texture_index(of)];
    Kind kind;
    if (!t->used || !from->used || !from->im || t->im || !kind_of(internal, &kind)) return;
    t->im = from->im, t->im->refs++;
    t->target = target, t->kind = kind, t->level0 = from->level0 + level0, t->levels = levels, t->layer0 = from->layer0 + layer0, t->layers = layers;
}
void vk_glCopyImageSubData(GLuint src, GLenum src_target, GLint src_level, GLint sx, GLint sy, GLint sz, GLuint dst, GLenum dst_target, GLint dst_level, GLint dx, GLint dy,
                           GLint dz, GLsizei w, GLsizei h, GLsizei d) {
    (void)src_target, (void)dst_target;
    Tex *a = &texs[texture_index(src)], *b = &texs[texture_index(dst)];
    if (!a->used || !b->used || !a->im || !b->im) return;
    {
        // (A copy that doesn't lie within both levels isn't done: OpenGL refuses one, and Vulkan, told to do it, loses the device.
        // A level's size is counted in whole blocks for a compressed texture; the box is in the source's texels.)
        const u32 al = a->level0 + (u32)src_level, bl = b->level0 + (u32)dst_level;
        if (al >= a->im->levels || bl >= b->im->levels) return;
        const u32 aw = a->im->w >> al ? a->im->w >> al : 1, ah = a->im->h >> al ? a->im->h >> al : 1, bw = b->im->w >> bl ? b->im->w >> bl : 1, bh = b->im->h >> bl ? b->im->h >> bl : 1;
        const u32 aw_ = (aw + a->kind.bw - 1) / a->kind.bw * a->kind.bw, ah_ = (ah + a->kind.bh - 1) / a->kind.bh * a->kind.bh;
        const u32 bw_ = (bw + b->kind.bw - 1) / b->kind.bw * b->kind.bw, bh_ = (bh + b->kind.bh - 1) / b->kind.bh * b->kind.bh;
        const u32 dw = (u32)w * b->kind.bw / a->kind.bw, dh = (u32)h * b->kind.bh / a->kind.bh;  // (what the box covers of the other texture)
        if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0 || (u32)sx + (u32)w > aw_ || (u32)sy + (u32)h > ah_ || (u32)dx + dw > bw_ || (u32)dy + dh > bh_ ||
            a->kind.bytes != b->kind.bytes) {
            SAY(8, "[vk] a copy between textures that doesn't fit them isn't done (%dx%d at %d,%d of %ux%u, format %d, to %d,%d of %ux%u, format %d)\n", (s64)w, (s64)h, (s64)sx,
                (s64)sy, (u64)aw, (u64)ah, (s64)a->kind.vk, (s64)dx, (s64)dy, (u64)bw, (u64)bh, (s64)b->kind.vk);
            return;
        }
    }
    image_layout(a->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_layout(b->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy copy = {.srcSubresource = {a->kind.depth ? aspect_of(a, 0) : VK_IMAGE_ASPECT_COLOR_BIT, a->level0 + (u32)src_level, a->im->is3d ? 0 : a->layer0 + (u32)sz, a->im->is3d ? 1 : (u32)d},
                              .srcOffset = {sx, sy, a->im->is3d ? sz : 0},
                              .dstSubresource = {b->kind.depth ? aspect_of(b, 0) : VK_IMAGE_ASPECT_COLOR_BIT, b->level0 + (u32)dst_level, b->im->is3d ? 0 : b->layer0 + (u32)dz, b->im->is3d ? 1 : (u32)d},
                              .dstOffset = {dx, dy, b->im->is3d ? dz : 0},
                              .extent = {(u32)w, (u32)h, a->im->is3d ? (u32)d : 1}};
    vkCmdCopyImage(cmd, a->im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// ---------------------------------------------------------------- samplers
static VkFilter filter_of(u32 f) { return f == GL_NEAREST || f == GL_NEAREST_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }
static VkSamplerAddressMode wrap_of(u32 w) {
    return w == GL_REPEAT ? VK_SAMPLER_ADDRESS_MODE_REPEAT : w == GL_MIRRORED_REPEAT ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT :
           w == GL_CLAMP_TO_BORDER ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
static VkCompareOp compare_of(u32 f) { return f >= GL_NEVER && f <= GL_ALWAYS ? (VkCompareOp)(f - GL_NEVER) : VK_COMPARE_OP_ALWAYS; }
static VkSampler sampler_make(u32 min, u32 mag, u32 ws, u32 wt, u32 wr, float lod_min, float lod_max, u32 compare, u32 func, const float *border) {
    const int mips = min != GL_NEAREST && min != GL_LINEAR;
    VkSamplerCustomBorderColorCreateInfoEXT custom = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT, .format = VK_FORMAT_UNDEFINED};
    int plain = !border || !custom_borders;
    if (border) {
        memcpy(custom.customBorderColor.float32, border, 16);
        if (border[0] == 0 && border[1] == 0 && border[2] == 0 && (border[3] == 0 || border[3] == 1)) plain = 1;
    }
    const VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = plain ? NULL : &custom, .magFilter = filter_of(mag), .minFilter = filter_of(min),
                                     .mipmapMode = min == GL_NEAREST_MIPMAP_LINEAR || min == GL_LINEAR_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                     .addressModeU = wrap_of(ws), .addressModeV = wrap_of(wt), .addressModeW = wrap_of(wr), .compareEnable = compare == GL_COMPARE_REF_TO_TEXTURE,
                                     .compareOp = compare_of(func), .minLod = mips ? lod_min : 0, .maxLod = mips ? lod_max : 0.25f,
                                     .borderColor = !plain ? VK_BORDER_COLOR_FLOAT_CUSTOM_EXT : border && border[3] == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
                                                    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler s = VK_NULL_HANDLE;
    CHECK("a sampler", vkCreateSampler(device, &sci, NULL, &s));
    return s;
}
void vk_glGenSamplers(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) {
        out[k] = 0;
        for (u32 id = 1; id < SAMPS; id++)
            if (!samps[id].used) {
                memset(&samps[id], 0, sizeof samps[id]);
                samps[id].used = 1, samps[id].min = GL_NEAREST_MIPMAP_LINEAR, samps[id].mag = GL_LINEAR, samps[id].ws = samps[id].wt = samps[id].wr = GL_REPEAT;
                samps[id].lod_min = -1000, samps[id].lod_max = 1000, samps[id].func = GL_LEQUAL;
                out[k] = id;
                break;
            }
    }
}
static void samp_changed(Samp *s) {
    if (s->vk) retire(4, (u64)s->vk, 0), s->vk = VK_NULL_HANDLE;
}
void vk_glSamplerParameteri(GLuint id, GLenum name, GLint value) {
    Samp *s = &samps[id < SAMPS ? id : 0];
    if (!s->used) return;
    u32 *what = name == GL_TEXTURE_MIN_FILTER ? &s->min : name == GL_TEXTURE_MAG_FILTER ? &s->mag : name == GL_TEXTURE_WRAP_S ? &s->ws : name == GL_TEXTURE_WRAP_T ? &s->wt :
                name == GL_TEXTURE_WRAP_R ? &s->wr : name == GL_TEXTURE_COMPARE_MODE ? &s->compare : name == GL_TEXTURE_COMPARE_FUNC ? &s->func : NULL;
    if (what) *what = (u32)value, samp_changed(s);
}
void vk_glSamplerParameterf(GLuint id, GLenum name, GLfloat value) {
    Samp *s = &samps[id < SAMPS ? id : 0];
    if (!s->used) return;
    if (name == GL_TEXTURE_MIN_LOD) s->lod_min = value, samp_changed(s);
    else if (name == GL_TEXTURE_MAX_LOD) s->lod_max = value, samp_changed(s);
}
void vk_glSamplerParameterfv(GLuint id, GLenum name, const GLfloat *value) {
    Samp *s = &samps[id < SAMPS ? id : 0];
    if (s->used && name == GL_TEXTURE_BORDER_COLOR) memcpy(s->border, value, 16), samp_changed(s);
}
void vk_glBindSampler(GLuint unit, GLuint id) { gl.unit_samp[unit & 31] = id < SAMPS && samps[id].used ? id : 0; }

// ---------------------------------------------------------------- buffers
void vk_glGenBuffers(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) {
        u32 id = 0;
        for (u32 tries = 0; tries < BUFS; tries++, buf_next = buf_next + 1 < BUFS ? buf_next + 1 : 1)
            if (!bufs[buf_next].used) {
                id = buf_next;
                break;
            }
        if (id) memset(&bufs[id], 0, sizeof bufs[id]), bufs[id].used = 1;
        out[k] = id;
    }
}
void vk_glDeleteBuffers(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++) {
        const u32 id = ids[k];
        if (!id || id >= BUFS || !bufs[id].used) continue;
        piece_give(bufs[id].piece);
        memset(&bufs[id], 0, sizeof bufs[id]);
    }
}
static u32 *bound_buffer(GLenum target) {
    return target == GL_ARRAY_BUFFER ? &gl.array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? &gl.element_buffer : target == GL_UNIFORM_BUFFER ? &gl.uniform_buffer : &gl.storage_buffer;
}
void vk_glBindBuffer(GLenum target, GLuint id) { *bound_buffer(target) = id < BUFS && bufs[id].used ? id : 0; }
// (A buffer that draws already recorded may read is given new memory before it is written, and the old is left for those draws:
// they haven't been run yet. One whose draws read only what lies before the bytes written, as a buffer filled a piece at a time
// is, is written where it is.)
static void buffer_fresh(Buf *b, u32 keep) {
    const Piece was = b->piece;
    b->piece = piece_take(b->size);
    if (!b->piece.size) SAY(8, "[vk] no memory for a buffer of %u bytes\n", (u64)b->size);
    if (b->piece.size && was.size && keep) memcpy(buf_at(b), blocks[was.block].at + was.at, keep < b->size ? keep : b->size);
    piece_give(was);
    b->read_top = 0;
}
void vk_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)usage;
    Buf *b = &bufs[*bound_buffer(target)];
    if (!b->used) return;
    const int in_use = b->read_top && serial - b->read_serial < FRAMES;
    if (!b->piece.size || (1ull << b->piece.size) < (u64)size || in_use) {
        b->size = (u32)size;
        buffer_fresh(b, 0);
    }
    b->size = (u32)size, b->read_top = 0;
    if (data && b->piece.size) memcpy(buf_at(b), data, (size_t)size);
}
void vk_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    Buf *b = &bufs[*bound_buffer(target)];
    if (!b->used || !b->piece.size || (u64)offset + (u64)size > b->size) return;
    if ((u32)offset < b->read_top && serial - b->read_serial < FRAMES) buffer_fresh(b, b->size);
    if (b->piece.size) memcpy(buf_at(b) + offset, data, (size_t)size);
}
void vk_glBindBufferRange(GLenum target, GLuint index, GLuint id, GLintptr offset, GLsizeiptr size) {
    if (target == GL_UNIFORM_BUFFER && index < 32) gl.ubo[index].buffer = id, gl.ubo[index].offset = (u32)offset, gl.ubo[index].size = (u32)size, gl.uniform_buffer = id;
    else if (target == GL_SHADER_STORAGE_BUFFER && index < 16) gl.ssbo[index].buffer = id, gl.ssbo[index].offset = (u32)offset, gl.ssbo[index].size = (u32)size, gl.storage_buffer = id;
}
void vk_glBindBufferBase(GLenum target, GLuint index, GLuint id) { vk_glBindBufferRange(target, index, id, 0, id < BUFS ? bufs[id].size : 0); }
// a buffer as a draw is recorded with it: which of Vulkan's, and where in it
static VkBuffer buffer_read(u32 id, u32 top, VkDeviceSize *at) {
    Buf *b = &bufs[id < BUFS ? id : 0];
    if (!b->used || !b->piece.size) return VK_NULL_HANDLE;
    if (top > b->read_top || b->read_serial != serial) b->read_top = top > b->read_top ? top : b->read_top;
    b->read_serial = serial;
    *at = b->piece.at;
    return blocks[b->piece.block].buffer;
}

// ---------------------------------------------------------------- vertex attributes
void vk_glGenVertexArrays(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) out[k] = 1;
}
void vk_glBindVertexArray(GLuint id) { (void)id; }
void vk_glEnableVertexAttribArray(GLuint k) {
    if (k < 16) gl.attrib[k].on = 1;
}
void vk_glDisableVertexAttribArray(GLuint k) {
    if (k < 16) gl.attrib[k].on = 0;
}
void vk_glVertexAttribPointer(GLuint k, GLint parts, GLenum type, GLboolean norm, GLsizei stride, const void *offset) {
    if (k >= 16) return;
    gl.attrib[k].parts = parts, gl.attrib[k].type = type, gl.attrib[k].norm = norm, gl.attrib[k].whole = 0, gl.attrib[k].stride = (u32)stride, gl.attrib[k].offset = (u64)offset;
    gl.attrib[k].buffer = gl.array_buffer;
}
void vk_glVertexAttribIPointer(GLuint k, GLint parts, GLenum type, GLsizei stride, const void *offset) {
    if (k >= 16) return;
    gl.attrib[k].parts = parts, gl.attrib[k].type = type, gl.attrib[k].norm = 0, gl.attrib[k].whole = 1, gl.attrib[k].stride = (u32)stride, gl.attrib[k].offset = (u64)offset;
    gl.attrib[k].buffer = gl.array_buffer;
}
void vk_glVertexAttribDivisor(GLuint k, GLuint divisor) {
    if (k < 16) gl.attrib[k].divisor = divisor;
}
void vk_glVertexAttribI4ui(GLuint k, GLuint x, GLuint y, GLuint z, GLuint w) { (void)k, (void)x, (void)y, (void)z, (void)w; }  // (an attribute left out reads zeros: see the draw)
// what an attribute is to Vulkan, and how the shader is to take it (0 a float's bits, 1 an unsigned whole number, 2 a signed one)
static VkFormat attrib_format(int parts, u32 type, int norm, int whole, int *how) {
    const int n = parts < 1 ? 0 : parts > 4 ? 3 : parts - 1;
    *how = 0;
    switch (type) {
    case GL_FLOAT: return (VkFormat[]){VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}[n];
    case GL_HALF_FLOAT: return (VkFormat[]){VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT}[n];
    case GL_UNSIGNED_INT:
    case GL_INT:  // (whole numbers of 32 bits: given as floats of the same bits, which is what the shader's registers hold)
        return (VkFormat[]){VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}[n];
    case GL_UNSIGNED_BYTE:
        if (whole) *how = 1;
        return (VkFormat)((whole ? VK_FORMAT_R8_USCALED : norm ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8_USCALED) + 7 * n);
    case GL_BYTE:
        if (whole) *how = 2;
        return (VkFormat)((whole ? VK_FORMAT_R8_SSCALED : norm ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_SSCALED) + 7 * n);
    case GL_UNSIGNED_SHORT:
        if (whole) *how = 1;
        return (VkFormat)((whole ? VK_FORMAT_R16_USCALED : norm ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16_USCALED) + 7 * n);
    case GL_SHORT:
        if (whole) *how = 2;
        return (VkFormat)((whole ? VK_FORMAT_R16_SSCALED : norm ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_SSCALED) + 7 * n);
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        if (whole) *how = 1;
        return whole ? VK_FORMAT_A2B10G10R10_USCALED_PACK32 : norm ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case GL_INT_2_10_10_10_REV:
        if (whole) *how = 2;
        return whole ? VK_FORMAT_A2B10G10R10_SSCALED_PACK32 : norm ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

// ---------------------------------------------------------------- framebuffers
void vk_glGenFramebuffers(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) {
        u32 id = 0;
        for (u32 tries = 0; tries < FBS; tries++, fb_next = fb_next + 1 < FBS ? fb_next + 1 : 1)
            if (!fbs[fb_next].used) {
                id = fb_next;
                break;
            }
        if (id) memset(&fbs[id], 0, sizeof fbs[id]), fbs[id].used = 1, fbs[id].draws = 1;
        out[k] = id;
    }
}
void vk_glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++) {
        const u32 id = ids[k];
        if (!id || id >= FBS) continue;
        if (rendering && rendering_fbo == id) rendering_end();
        if (gl.fb_draw == id) gl.fb_draw = 0;
        if (gl.fb_read == id) gl.fb_read = 0;
        fbs[id].used = 0;
    }
}
void vk_glBindFramebuffer(GLenum target, GLuint id) {
    if (id >= FBS || (id && !fbs[id].used)) id = 0;
    if (target != GL_READ_FRAMEBUFFER) gl.fb_draw = id;
    if (target != GL_DRAW_FRAMEBUFFER) gl.fb_read = id;
}
static void attach(u32 point, u32 tex, u32 level, u32 layer) {
    Fb *f = &fbs[gl.fb_draw];
    if (!gl.fb_draw) return;
    if (rendering && rendering_fbo == gl.fb_draw) rendering_end();
    if (point >= GL_COLOR_ATTACHMENT0 && point < GL_COLOR_ATTACHMENT0 + 8) f->color[point - GL_COLOR_ATTACHMENT0].tex = texture_index(tex), f->color[point - GL_COLOR_ATTACHMENT0].level = level, f->color[point - GL_COLOR_ATTACHMENT0].layer = layer;
    else if (point == GL_DEPTH_ATTACHMENT || point == GL_DEPTH_STENCIL_ATTACHMENT) f->depth.tex = texture_index(tex), f->depth.level = level, f->depth.layer = layer, f->stencil = point == GL_DEPTH_STENCIL_ATTACHMENT;
}
void vk_glFramebufferTexture2D(GLenum target, GLenum point, GLenum kind, GLuint tex, GLint level) {
    (void)target;
    attach(point, tex, (u32)level, kind >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && kind < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6 ? kind - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0);
}
void vk_glFramebufferTextureLayer(GLenum target, GLenum point, GLuint tex, GLint level, GLint layer) {
    (void)target;
    attach(point, tex, (u32)level, (u32)layer);
}
void vk_glDrawBuffers(GLsizei n, const GLenum *which) {
    Fb *f = &fbs[gl.fb_draw];
    if (!gl.fb_draw) return;
    f->draws = 0;
    for (int k = 0; k < n && k < 8; k++)
        if (which[k] != GL_NONE) f->draws |= 1u << k;
}
void vk_glReadBuffer(GLenum which) { (void)which; }
GLenum vk_glCheckFramebufferStatus(GLenum target) {
    (void)target;
    return GL_FRAMEBUFFER_COMPLETE;
}
// the targets of a framebuffer, as Vulkan is told of them when drawing into it begins
typedef struct {
    u32 n, w, h;
    VkFormat color[8], depth;
    int stencil;
    VkImageView color_view[8], depth_view;
    Image *color_image[8], *depth_image;
} Targets;
static void targets_of(u32 id, Targets *out) {
    memset(out, 0, sizeof *out);
    if (!id) {
        Tex *t = &texs[window_tex];
        out->n = 1, out->w = t->im->w, out->h = t->im->h, out->color[0] = t->kind.vk, out->color_view[0] = tex_target(t, 0, 0), out->color_image[0] = t->im;
        return;
    }
    const Fb *f = &fbs[id];
    for (u32 k = 0; k < 8; k++) {
        Tex *t = &texs[f->color[k].tex];
        if (!f->color[k].tex || !t->used || !t->im || !t->im->attachable || !(f->draws >> k & 1)) continue;
        const u32 level = f->color[k].level < t->levels ? f->color[k].level : t->levels - 1;
        out->n = k + 1, out->color[k] = t->kind.vk, out->color_view[k] = tex_target(t, level, f->color[k].layer), out->color_image[k] = t->im;
        out->w = t->im->w >> (t->level0 + level) ? t->im->w >> (t->level0 + level) : 1, out->h = t->im->h >> (t->level0 + level) ? t->im->h >> (t->level0 + level) : 1;
    }
    Tex *d = &texs[f->depth.tex];
    if (f->depth.tex && d->used && d->im && d->im->attachable && d->kind.depth) {
        out->depth = d->kind.vk, out->stencil = d->kind.depth == 2, out->depth_view = tex_target(d, f->depth.level, f->depth.layer), out->depth_image = d->im;
        if (!out->w) out->w = d->im->w >> f->depth.level ? d->im->w >> f->depth.level : 1, out->h = d->im->h >> f->depth.level ? d->im->h >> f->depth.level : 1;
    }
}
static int rendering_begin(const Targets *t) {
    if (rendering && rendering_fbo == gl.fb_draw) return 1;
    rendering_end();
    if (!t->w || !t->h) return 0;
    for (u32 k = 0; k < t->n; k++)
        if (t->color_image[k]) image_layout(t->color_image[k], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (t->depth_image) image_layout(t->depth_image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo colors[8];
    for (u32 k = 0; k < t->n; k++)
        colors[k] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->color_view[k], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->depth_view, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                             .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {t->w, t->h}}, .layerCount = 1, .colorAttachmentCount = t->n, .pColorAttachments = colors,
                                .pDepthAttachment = t->depth_view ? &depth : NULL, .pStencilAttachment = t->depth_view && t->stencil ? &depth : NULL};
    vkCmdBeginRendering(cmd, &ri);
    rendering = 1, rendering_fbo = gl.fb_draw, rendering_w = t->w, rendering_h = t->h;
    return 1;
}
static void clear(u32 aspect, u32 index, const VkClearValue *value) {
    Targets t;
    targets_of(gl.fb_draw, &t);
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT ? index >= t.n || !t.color_view[index] : !t.depth_view) return;
    if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT && !t.stencil) aspect &= ~(u32)VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect || !rendering_begin(&t)) return;
    VkClearRect rect = {{{0, 0}, {t.w, t.h}}, 0, 1};
    if (gl.scissor_on) {
        int x0 = scissor.x < 0 ? 0 : scissor.x, y0 = scissor.y < 0 ? 0 : scissor.y, x1 = scissor.x + scissor.w, y1 = scissor.y + scissor.h;
        if (x1 > (int)t.w) x1 = (int)t.w;
        if (y1 > (int)t.h) y1 = (int)t.h;
        if (x1 <= x0 || y1 <= y0) return;
        rect.rect = (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    const VkClearAttachment what = {aspect, index, *value};
    vkCmdClearAttachments(cmd, 1, &what, 1, &rect);
}
void vk_glClearBufferfv(GLenum buffer, GLint index, const GLfloat *value) {
    VkClearValue v;
    if (buffer == GL_COLOR) {
        memcpy(v.color.float32, value, 16);
        clear(VK_IMAGE_ASPECT_COLOR_BIT, (u32)index, &v);
    } else if (buffer == GL_DEPTH) {
        v.depthStencil = (VkClearDepthStencilValue){value[0], 0};
        clear(VK_IMAGE_ASPECT_DEPTH_BIT, 0, &v);
    }
}
void vk_glClearBufferiv(GLenum buffer, GLint index, const GLint *value) {
    VkClearValue v;
    if (buffer == GL_STENCIL) {
        v.depthStencil = (VkClearDepthStencilValue){0, (u32)value[0]};
        clear(VK_IMAGE_ASPECT_STENCIL_BIT, 0, &v);
    } else if (buffer == GL_COLOR) {
        memcpy(v.color.int32, value, 16);
        clear(VK_IMAGE_ASPECT_COLOR_BIT, (u32)index, &v);
    }
}
void vk_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    (void)mask;
    Tex *a = &texs[gl.fb_read ? fbs[gl.fb_read].color[0].tex : window_tex], *b = &texs[gl.fb_draw ? fbs[gl.fb_draw].color[0].tex : window_tex];
    if (!a->used || !b->used || !a->im || !b->im || a->im == b->im) return;
    image_layout(a->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_layout(b->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, a->level0, a->layer0, 1}, .srcOffsets = {{sx0, sy0, 0}, {sx1, sy1, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, b->level0, b->layer0, 1}, .dstOffsets = {{dx0, dy0, 0}, {dx1, dy1, 1}}};
    vkCmdBlitImage(cmd, a->im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}
void vk_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out) {
    (void)x, (void)y;
    memset(out, 0, (size_t)w * (size_t)h * (type == GL_FLOAT ? 16 : 4));
    (void)format;
    SAY(1, "[vk] a pixel is read back: not done over Vulkan yet, zeros are given\n");
}

// ---------------------------------------------------------------- programs and pipelines
static Shader *shader_of(u64 key, u32 stage) {
    u32 lo = 0, hi = spv_count;
    while (lo < hi) {  // (the rows are in order of number, then stage)
        const u32 mid = (lo + hi) / 2;
        if (spv_rows[mid].key < key || (spv_rows[mid].key == key && spv_rows[mid].stage < stage)) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= spv_count || spv_rows[lo].key != key || spv_rows[lo].stage != stage) return NULL;
    Shader *s = &shaders[lo];
    if (s->module) return s;
    u32 *code = malloc(spv_rows[lo].size);
    if (!code || fseek(spv_file, (long)spv_rows[lo].at, SEEK_SET) || fread(code, 1, spv_rows[lo].size, spv_file) != spv_rows[lo].size) {
        free(code);
        return NULL;
    }
    const VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_rows[lo].size, .pCode = code};
    const VkResult r = vkCreateShaderModule(device, &ci, NULL, &s->module);
    free(code);
    if (r < 0) {
        SAY(20, "[vk] shader %x isn't taken by the driver: result %d\n", key, (s64)r);
        s->module = VK_NULL_HANDLE;
        return NULL;
    }
    s->key = key, s->stage = stage, s->storage = spv_rows[lo].storage, s->own = spv_rows[lo].own, s->between = spv_rows[lo].between;
    memcpy(s->blocks, spv_rows[lo].blocks, sizeof s->blocks), memcpy(s->textures, spv_rows[lo].textures, sizeof s->textures);
    return s;
}
// A program of two of the port's shaders, by their numbers: 0 when either isn't in the file. What nvn2.c asks of OpenGL about a
// program it has linked is given here straight: each uniform block's size (by the binding point, a stage's sixteen in a row),
// and for each texture unit, in the order nvn2.c numbers them, the stage, NVN's binding and OpenGL's name for the kind.
u32 glvk_program(u64 vs_key, u64 fs_key, u32 packed, u32 need[32], u8 units[24][2], u32 unit_kind[24], int *nunits, u32 *storage, u16 *attrs) {
    // (packed: the draw's colors are a compressed texture's blocks, whole numbers: the fragment shader's form that gives them so)
    Shader *vs = shader_of(vs_key, 1), *fs = packed ? shader_of(fs_key, 6) : NULL;
    if (packed && !fs) nx2_log("[vk] fragment shader %x is wanted with its colors as whole numbers, and the port's file hasn't it so (tools/spirv.py whole)\n", fs_key);
    if (!fs) fs = shader_of(fs_key, 5);
    if (!vs || !fs) return 0;
    u32 id = 0;
    for (u32 tries = 0; tries < PROGS; tries++, prog_next = prog_next + 1 < PROGS ? prog_next + 1 : 1)
        if (!progs[prog_next].used) {
            id = prog_next;
            break;
        }
    if (!id) return 0;
    Prog *p = &progs[id];
    memset(p, 0, sizeof *p);
    p->used = 1, p->vs = vs, p->fs = fs;
    VkDescriptorSetLayoutBinding bindings[80];
    u32 n = 0;
    *nunits = 0, *storage = 0;
    for (u32 stage = 0; stage < 2; stage++) {
        const Shader *s = stage ? fs : vs;
        const VkShaderStageFlags flag = stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        for (u32 k = 0; k < 16; k++) {
            need[stage * 16 + k] = s->blocks[k];
            if (s->blocks[k]) bindings[n++] = (VkDescriptorSetLayoutBinding){stage * 16 + k, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flag, NULL};
        }
        for (u32 k = 0; k < 32 && *nunits < 24; k++) {
            if (!s->textures[k]) continue;
            static const u32 names[10] = {0, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY,
                                          GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_ARRAY};
            const u32 binding = (stage ? 64 : 32) + k;
            bindings[n++] = (VkDescriptorSetLayoutBinding){binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flag, NULL};
            p->unit_binding[*nunits] = (u8)binding, p->unit_kind[*nunits] = s->textures[k];
            units[*nunits][0] = (u8)stage, units[*nunits][1] = (u8)k, unit_kind[*nunits] = names[s->textures[k] < 10 ? s->textures[k] : 1];
            (*nunits)++;
        }
        for (u32 k = 0; k < 8; k++)
            if (s->storage >> k & 1) bindings[n++] = (VkDescriptorSetLayoutBinding){96 + stage * 8 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flag, NULL}, (*storage)++;
    }
    p->units = (u32)*nunits;
    *attrs = (u16)vs->own;
    const VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                  .bindingCount = n, .pBindings = bindings};
    VkResult r = vkCreateDescriptorSetLayout(device, &slci, NULL, &p->set_layout);
    const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p->set_layout};
    if (r >= 0) r = vkCreatePipelineLayout(device, &plci, NULL, &p->layout);
    if (r < 0) {
        SAY(20, "[vk] a program's layout (%u bindings) isn't made: result %d\n", (u64)n, (s64)r);
        p->used = 0;
        return 0;
    }
    return id;
}
void vk_glUseProgram(GLuint id) { gl.program = id < PROGS && progs[id].used ? id : 0; }
void vk_glDeleteProgram(GLuint id) {
    if (!id || id >= PROGS || !progs[id].used) return;
    if (gl.program == id) gl.program = 0;
    // (its pipelines stay in their table, under its number and shaders: the next program of the same shaders has them again)
    vkDestroyPipelineLayout(device, progs[id].layout, NULL);
    vkDestroyDescriptorSetLayout(device, progs[id].set_layout, NULL);
    progs[id].used = 0;
}

typedef struct {
    u64 vs, fs;
    u32 whole;        // two bits an attribute: how the shader takes it
    u8 topology, ncolor, depth_zero, fs_stage;
    u32 color[8], depth;
} PipeKey;
#define PIPES 8192
static struct {
    PipeKey key;
    VkPipeline pipeline;
} pipes[PIPES];
static u32 npipes;
static u64 pipeline_time;
// The pipelines a game is drawn with, kept on the card: the driver's own store of what it compiled for them (a pipeline cache of
// Vulkan's, <folder>/vkc), and a list of which they were (<name>.vkp). At the next start they are all made again before
// the first picture, from the store: quickly, and not in the middle of a race. (Each took some 35 ms the first time it was drawn
// with, 400 of them in one race of a game: fourteen seconds of standing still, a little at a time; 2026-11-24.)
static VkPipelineCache pipeline_cache;
static char cache_path[80], list_path[80];
static u32 npipes_saved;
static int listing = 1;  // a pipeline newly made is added to the list (not while the list itself is being read)
static VkPipeline pipeline_for(const Prog *p, const PipeKey *key) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 k = 0; k < sizeof *key; k++) h = (h ^ ((const u8 *)key)[k]) * 0x100000001B3ull;
    u32 at = (u32)(h >> 20) % PIPES;
    for (u32 tries = 0; tries < PIPES; tries++, at = (at + 1) % PIPES) {
        if (!pipes[at].pipeline) break;
        if (!memcmp(&pipes[at].key, key, sizeof *key)) return pipes[at].pipeline;
    }
    if (npipes >= PIPES - 1) return VK_NULL_HANDLE;
    const u64 from = armGetSystemTick();
    int whole[17];
    VkSpecializationMapEntry entries[17];
    u32 n = 0;
    for (u32 k = 0; k < 16; k++)
        if (p->vs->own >> k & 1) whole[n] = (int)(key->whole >> (2 * k) & 3), entries[n] = (VkSpecializationMapEntry){k, n * 4, 4}, n++;
    whole[n] = !key->depth_zero, entries[n] = (VkSpecializationMapEntry){100, n * 4, 4}, n++;
    const VkSpecializationInfo special = {n, entries, n * 4, whole};
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs->module, .pName = "main", .pSpecializationInfo = &special},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->fs->module, .pName = "main"}};
    const VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)key->topology};
    const VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend[8];
    for (int k = 0; k < 8; k++) blend[k] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 15};
    const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = key->ncolor, .pAttachments = blend};
    static const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
    const VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dynamic / sizeof *dynamic, .pDynamicStates = dynamic};
    VkFormat color[8];
    for (int k = 0; k < 8; k++) color[k] = (VkFormat)key->color[k];
    const int stencil = key->depth == VK_FORMAT_D24_UNORM_S8_UINT || key->depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkPipelineRenderingCreateInfo rendering_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = key->ncolor, .pColorAttachmentFormats = color,
                                                      .depthAttachmentFormat = (VkFormat)key->depth, .stencilAttachmentFormat = stencil ? (VkFormat)key->depth : VK_FORMAT_UNDEFINED};
    const VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering_, .stageCount = 2, .pStages = stages, .pInputAssemblyState = &ia,
                                             .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                             .pDynamicState = &dy, .layout = p->layout};
    VkPipeline made = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &ci, NULL, &made);
    pipeline_time += armGetSystemTick() - from;
    if (r >= 0 && listing && list_path[0]) {
        FILE *f = fopen(list_path, "ab");
        if (f) fwrite(key, sizeof *key, 1, f), fclose(f);
    }
    if (r < 0) {
        SAY(20, "[vk] a pipeline for shaders %x, %x isn't made: result %d\n", key->vs, key->fs, (s64)r);
        return VK_NULL_HANDLE;
    }
    pipes[at].key = *key, pipes[at].pipeline = made, npipes++;
    if (npipes % 100 == 0) nx2_log("[vk] %u pipelines made so far, %u ms in all\n", (u64)npipes, armTicksToNs(pipeline_time) / 1000000);
    return made;
}

static void pipelines_at_start(const char *pack) {
    const size_t n = strlen(pack);
    if (n < 5 || n + 1 > sizeof cache_path) return;
    memcpy(cache_path, pack, n + 1), memcpy(list_path, pack, n + 1);
    memcpy(cache_path + n - 3, "vkc", 3), memcpy(list_path + n - 3, "vkp", 3);
    void *data = NULL;
    size_t size = 0;
    FILE *f = fopen(cache_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        data = size ? malloc(size) : NULL;
        if (!data || fread(data, 1, size, f) != size) size = 0;
        fclose(f);
    }
    const VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size, .pInitialData = data};
    CHECK("the store of pipelines", vkCreatePipelineCache(device, &ci, NULL, &pipeline_cache));
    free(data);
    const u64 from = armGetSystemTick();
    u32 listed = 0, made = 0;
    f = fopen(list_path, "rb");
    if (f) {
        PipeKey key;
        listing = 0;
        while (fread(&key, sizeof key, 1, f) == 1) {
            listed++;
            Shader *vs = shader_of(key.vs, 1), *fs = shader_of(key.fs, key.fs_stage == 6 ? 6 : 5);
            if (!vs || !fs) continue;
            // (a layout as glvk_program makes a program's: pipelines made for one are good for any that is alike)
            Prog p = {.used = 1, .vs = vs, .fs = fs};
            VkDescriptorSetLayoutBinding bindings[80];
            u32 nb = 0;
            for (u32 stage = 0; stage < 2; stage++) {
                const Shader *x = stage ? fs : vs;
                const VkShaderStageFlags flag = stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                for (u32 k = 0; k < 16; k++)
                    if (x->blocks[k]) bindings[nb++] = (VkDescriptorSetLayoutBinding){stage * 16 + k, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flag, NULL};
                for (u32 k = 0, units = 0; k < 32 && units < 24; k++)
                    if (x->textures[k]) bindings[nb++] = (VkDescriptorSetLayoutBinding){(stage ? 64 : 32) + k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flag, NULL}, units++;
                for (u32 k = 0; k < 8; k++)
                    if (x->storage >> k & 1) bindings[nb++] = (VkDescriptorSetLayoutBinding){96 + stage * 8 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, flag, NULL};
            }
            const VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                          .bindingCount = nb, .pBindings = bindings};
            if (vkCreateDescriptorSetLayout(device, &slci, NULL, &p.set_layout) < 0) continue;
            const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.set_layout};
            if (vkCreatePipelineLayout(device, &plci, NULL, &p.layout) >= 0) {
                if (pipeline_for(&p, &key)) made++;
                vkDestroyPipelineLayout(device, p.layout, NULL);
            }
            vkDestroyDescriptorSetLayout(device, p.set_layout, NULL);
        }
        listing = 1;
        fclose(f);
    }
    npipes_saved = npipes;
    nx2_log("[vk] pipelines kept on the card: a store of %u bytes, %u listed, %u made again in %u ms\n", (u64)size, (u64)listed, (u64)made,
            armTicksToNs(armGetSystemTick() - from) / 1000000);
    pipeline_time = 0;
}
// the driver's store is written to the card when pipelines have been made since it last was, and none for a while
static void pipelines_keep(void) {
    static u32 last_count, quiet;
    if (!pipeline_cache || !cache_path[0]) return;
    if (npipes != last_count) last_count = npipes, quiet = 0;
    else quiet++;
    if (npipes == npipes_saved || quiet < 300) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, NULL) < 0 || !size) return;
    void *data = malloc(size);
    if (data && vkGetPipelineCacheData(device, pipeline_cache, &size, data) >= 0) {
        FILE *f = fopen(cache_path, "wb");
        if (f) fwrite(data, 1, size, f), fclose(f);
        nx2_log("[vk] the store of pipelines is written to the card: %u bytes, for %u pipelines\n", (u64)size, (u64)npipes);
    }
    free(data);
    npipes_saved = npipes;
}

// ---------------------------------------------------------------- state
void vk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { viewport.x = x, viewport.y = y, viewport.w = w, viewport.h = h; }
void vk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { scissor.x = x, scissor.y = y, scissor.w = w, scissor.h = h; }
void vk_glDepthRangef(GLfloat n, GLfloat f) { gl.near = n, gl.far = f; }
static int *cap(GLenum what) {
    return what == GL_SCISSOR_TEST ? &gl.scissor_on : what == GL_DEPTH_TEST ? &gl.depth_test : what == GL_STENCIL_TEST ? &gl.stencil_test : what == GL_CULL_FACE ? &gl.cull_on :
           what == GL_POLYGON_OFFSET_FILL ? &gl.offset_on : what == GL_BLEND ? (int *)&gl.blend[0].on : NULL;
}
void vk_glEnable(GLenum what) {
    if (what == GL_BLEND)
        for (int k = 0; k < 8; k++) gl.blend[k].on = 1;
    else if (cap(what)) *cap(what) = 1;
}
void vk_glDisable(GLenum what) {
    if (what == GL_BLEND)
        for (int k = 0; k < 8; k++) gl.blend[k].on = 0;
    else if (cap(what)) *cap(what) = 0;
}
void vk_glEnablei(GLenum what, GLuint k) {
    if (what == GL_BLEND && k < 8) gl.blend[k].on = 1;
}
void vk_glDisablei(GLenum what, GLuint k) {
    if (what == GL_BLEND && k < 8) gl.blend[k].on = 0;
}
GLboolean vk_glIsEnabled(GLenum what) { return cap(what) ? (GLboolean)*cap(what) : 0; }
void vk_glDepthFunc(GLenum f) { gl.depth_func = f; }
void vk_glDepthMask(GLboolean on) { gl.depth_write = on; }
void vk_glCullFace(GLenum f) { gl.cull_face = f; }
void vk_glFrontFace(GLenum f) { gl.front_face = f; }
void vk_glPolygonOffset(GLfloat factor, GLfloat units) { gl.offset_factor = factor, gl.offset_units = units; }
void vk_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { gl.blend_color[0] = r, gl.blend_color[1] = g, gl.blend_color[2] = b, gl.blend_color[3] = a; }
void vk_glColorMaski(GLuint k, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (k < 8) gl.blend[k].mask = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
}
void vk_glBlendFuncSeparatei(GLuint k, GLenum src, GLenum dst, GLenum srca, GLenum dsta) {
    if (k < 8) gl.blend[k].src = src, gl.blend[k].dst = dst, gl.blend[k].srca = srca, gl.blend[k].dsta = dsta;
}
void vk_glBlendEquationSeparatei(GLuint k, GLenum eq, GLenum eqa) {
    if (k < 8) gl.blend[k].eq = eq, gl.blend[k].eqa = eqa;
}
void vk_glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    for (int k = 0; k < 2; k++)
        if (face == GL_FRONT_AND_BACK || face == (k ? GL_BACK : GL_FRONT)) gl.stencil[k].func = func, gl.stencil[k].ref = (u32)ref, gl.stencil[k].value_mask = mask;
}
void vk_glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) {
    for (int k = 0; k < 2; k++)
        if (face == GL_FRONT_AND_BACK || face == (k ? GL_BACK : GL_FRONT)) gl.stencil[k].fail = fail, gl.stencil[k].zfail = zfail, gl.stencil[k].zpass = zpass;
}
void vk_glStencilMaskSeparate(GLenum face, GLuint mask) {
    for (int k = 0; k < 2; k++)
        if (face == GL_FRONT_AND_BACK || face == (k ? GL_BACK : GL_FRONT)) gl.stencil[k].write_mask = mask;
}
void vk_glStencilMask(GLuint mask) { gl.stencil[0].write_mask = gl.stencil[1].write_mask = mask; }
GLenum vk_glGetError(void) { return 0; }
const GLubyte *vk_glGetString(GLenum name) {
    return (const GLubyte *)(name == GL_VERSION ? "OpenGL ES 3.2 as far as nx2 uses it, over Vulkan" : name == GL_RENDERER ? "nx2 glvk" : name == GL_EXTENSIONS ? "GL_EXT_clip_control" : "3.20");
}
void vk_glGetIntegerv(GLenum name, GLint *out) {
    switch (name) {
    case GL_DRAW_FRAMEBUFFER_BINDING: *out = (GLint)gl.fb_draw; break;  // (GL_FRAMEBUFFER_BINDING is the same number)
    case GL_READ_FRAMEBUFFER_BINDING: *out = (GLint)gl.fb_read; break;
    case GL_VIEWPORT: out[0] = viewport.x, out[1] = viewport.y, out[2] = viewport.w, out[3] = viewport.h; break;
    case GL_SCISSOR_BOX: out[0] = scissor.x, out[1] = scissor.y, out[2] = scissor.w, out[3] = scissor.h; break;
    case GL_BLEND_SRC_RGB: *out = (GLint)gl.blend[0].src; break;
    case GL_BLEND_DST_RGB: *out = (GLint)gl.blend[0].dst; break;
    case GL_BLEND_SRC_ALPHA: *out = (GLint)gl.blend[0].srca; break;
    case GL_BLEND_DST_ALPHA: *out = (GLint)gl.blend[0].dsta; break;
    case GL_BLEND_EQUATION_RGB: *out = (GLint)gl.blend[0].eq; break;
    case GL_DEPTH_FUNC: *out = (GLint)gl.depth_func; break;
    case GL_CULL_FACE_MODE: *out = (GLint)gl.cull_face; break;
    case GL_FRONT_FACE: *out = (GLint)gl.front_face; break;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: *out = GL_UNSIGNED_BYTE; break;
    default: *out = 0;
    }
}
void vk_glGetBooleanv(GLenum name, GLboolean *out) {
    if (name == GL_DEPTH_WRITEMASK) *out = (GLboolean)gl.depth_write;
    else if (name == GL_COLOR_WRITEMASK) out[0] = gl.blend[0].mask & 1, out[1] = gl.blend[0].mask >> 1 & 1, out[2] = gl.blend[0].mask >> 2 & 1, out[3] = gl.blend[0].mask >> 3 & 1;
    else *out = 0;
}
void glvk_flip(int on) { gl.flip = on; }             // what a shader puts at the top of clip space goes to a target's first row (NVN's window origin at the upper left)
void glvk_depth_zero(int on) { gl.depth_zero = on; } // clip space's depth runs from 0, not from -w

// ---------------------------------------------------------------- draws
static VkBlendFactor factor_of(u32 f) {
    switch (f) {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp equation_of(u32 e) {
    return e == GL_FUNC_SUBTRACT ? VK_BLEND_OP_SUBTRACT : e == GL_FUNC_REVERSE_SUBTRACT ? VK_BLEND_OP_REVERSE_SUBTRACT : e == GL_MIN ? VK_BLEND_OP_MIN : e == GL_MAX ? VK_BLEND_OP_MAX : VK_BLEND_OP_ADD;
}
static VkStencilOp stencil_of(u32 o) {
    return o == GL_ZERO ? VK_STENCIL_OP_ZERO : o == GL_REPLACE ? VK_STENCIL_OP_REPLACE : o == GL_INCR ? VK_STENCIL_OP_INCREMENT_AND_CLAMP : o == GL_DECR ? VK_STENCIL_OP_DECREMENT_AND_CLAMP :
           o == GL_INVERT ? VK_STENCIL_OP_INVERT : o == GL_INCR_WRAP ? VK_STENCIL_OP_INCREMENT_AND_WRAP : o == GL_DECR_WRAP ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_KEEP;
}
static VkBuffer zeros_buffer(VkDeviceSize *at) {  // (sixteen zero bytes, for an attribute a draw leaves out)
    static u32 id;
    if (!id) {
        static const u8 zeros[64];
        const u32 was = gl.array_buffer;
        vk_glGenBuffers(1, &id);
        gl.array_buffer = id;
        vk_glBufferData(GL_ARRAY_BUFFER, sizeof zeros, zeros, GL_STATIC_DRAW);
        gl.array_buffer = was;
    }
    return buffer_read(id, 0, at);
}
static u32 n_draws, n_left;
static u64 took[8];  // ticks in a draw's parts: its textures, its pipeline, its vertices, its descriptors, the rest said, the draw itself, showing
#define TICK(k) \
    do { \
        const u64 now_ = armGetSystemTick(); \
        took[k] += now_ - tick_, tick_ = now_; \
    } while (0)
// What the command buffer has been told so far: a draw says again only what differs from it. (Every draw said everything: seven
// microseconds a draw, measured in a game's races, 2026-11-24.)
static struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    u32 nstreams;
    struct {
        u32 format, stride, rate, location;
    } stream[16];
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    struct {
        u64 a, b, c;
    } bound[128];
    VkViewport viewport;
    VkRect2D cut;
    u32 cull, front, depth_test, depth_write, depth_func, stencil_test, offset_on, ncolor;
    float offset_units, offset_factor, blend_color[4];
    u32 stencil[2][7];
    struct {
        u32 on, src, dst, srca, dsta, eq, eqa, mask;
    } blend[8];
    int known;
} told;
// all that a draw needs, said to Vulkan: 0 when it can't be drawn
static int draw_ready(u32 mode) {
    const Prog *p = &progs[gl.program];
    if (!gl.program || !p->used) return 0;
    u64 tick_ = armGetSystemTick();
    // the textures it reads are made ready to be read before drawing begins (a change of an image's layout ends a pass)
    Targets t;
    targets_of(gl.fb_draw, &t);
    if (!t.w) return 0;
    VkDescriptorImageInfo images[24];
    for (u32 u = 0; u < p->units; u++) {
        Tex *x = &texs[gl.unit_tex[u]];
        VkImageView view = x->used && x->im ? tex_view(x) : VK_NULL_HANDLE;
        if (!view) {
            SAY(12, "[vk] a draw reads a texture that isn't there (unit %u, binding %u): left out\n", (u64)u, (u64)p->unit_binding[u]);
            return 0;
        }
        int target = 0;
        for (u32 k = 0; k < t.n; k++) target |= t.color_image[k] == x->im;
        target |= t.depth_image == x->im;
        if (!target) image_layout(x->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkSampler s;
        Samp *bound = &samps[gl.unit_samp[u]];
        if (gl.unit_samp[u] && bound->used) {
            if (!bound->vk) bound->vk = sampler_make(bound->min, bound->mag, bound->ws, bound->wt, bound->wr, bound->lod_min, bound->lod_max, bound->compare, bound->func, bound->border);
            s = bound->vk;
        } else {
            if (!x->sampler) x->sampler = sampler_make(x->min, x->mag, x->ws, x->wt, x->wr, -1000, 1000, 0, 0, NULL);
            s = x->sampler;
        }
        images[u] = (VkDescriptorImageInfo){s, view, x->im->layout};
    }
    TICK(0);
    PipeKey key;
    memset(&key, 0, sizeof key);
    key.vs = p->vs->key, key.fs = p->fs->key, key.fs_stage = (u8)p->fs->stage, key.ncolor = (u8)t.n, key.depth = t.depth, key.depth_zero = (u8)gl.depth_zero;
    for (u32 k = 0; k < t.n; k++) key.color[k] = t.color[k];
    key.topology = (u8)(mode == GL_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : mode == GL_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : mode == GL_LINE_STRIP || mode == GL_LINE_LOOP ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP :
                        mode == GL_TRIANGLE_STRIP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : mode == GL_TRIANGLE_FAN ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    // the vertex layout: a stream of Vulkan's for each attribute the shader reads
    VkVertexInputBindingDescription2EXT streams[16];
    VkVertexInputAttributeDescription2EXT attributes[16];
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    u32 n = 0;
    for (u32 k = 0; k < 16; k++) {
        if (!(p->vs->own >> k & 1)) continue;
        int how = 0;
        VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
        u32 stride = 0, divisor = 0;
        VkDeviceSize at = 0;
        VkBuffer b = VK_NULL_HANDLE;
        if (gl.attrib[k].on) {
            format = attrib_format(gl.attrib[k].parts, gl.attrib[k].type, gl.attrib[k].norm, gl.attrib[k].whole, &how);
            b = buffer_read(gl.attrib[k].buffer, ~0u, &at);
            stride = gl.attrib[k].stride, divisor = gl.attrib[k].divisor, at += gl.attrib[k].offset;
        }
        if (!b) b = zeros_buffer(&at), stride = 0, divisor = 0, format = VK_FORMAT_R32G32B32A32_SFLOAT, how = 0;
        key.whole |= (u32)how << (2 * k);
        streams[n] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = n, .stride = stride,
                                                           .inputRate = divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
        attributes[n] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = k, .binding = n, .format = format};
        buffers[n] = b, offsets[n] = at, n++;
    }
    TICK(2);
    const VkPipeline pipeline = pipeline_for(p, &key);
    if (!pipeline || !rendering_begin(&t)) return 0;
    TICK(1);
    if (gl.fresh) memset(&told, 0, sizeof told), gl.fresh = 0;
    const int all = !told.known;
    told.known = 1;
    if (all || told.pipeline != pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline), told.pipeline = pipeline;
    {
        int same = !all && told.nstreams == n;
        for (u32 k = 0; same && k < n; k++)
            same = told.stream[k].format == (u32)attributes[k].format && told.stream[k].stride == streams[k].stride && told.stream[k].rate == (u32)streams[k].inputRate &&
                   told.stream[k].location == attributes[k].location;
        if (!same) {
            set_vertex_input(cmd, n, streams, n, attributes);
            told.nstreams = n;
            for (u32 k = 0; k < n; k++)
                told.stream[k].format = (u32)attributes[k].format, told.stream[k].stride = streams[k].stride, told.stream[k].rate = (u32)streams[k].inputRate,
                told.stream[k].location = attributes[k].location;
        }
        if (n && (!same || memcmp(told.buffers, buffers, n * sizeof *buffers) || memcmp(told.offsets, offsets, n * sizeof *offsets))) {
            vkCmdBindVertexBuffers(cmd, 0, n, buffers, offsets);
            memcpy(told.buffers, buffers, n * sizeof *buffers), memcpy(told.offsets, offsets, n * sizeof *offsets);
        }
    }
    TICK(2);
    // (pushed descriptors last while the layouts they are pushed for are alike: another program's layout, and all are pushed again)
    const int every = all || told.layout != p->layout;
    if (every) memset(told.bound, 0, sizeof told.bound), told.layout = p->layout;
    // the descriptors
    VkWriteDescriptorSet writes[64];
    VkDescriptorBufferInfo ranges[48];
    u32 nw = 0, nr = 0;
    for (u32 k = 0; k < 32; k++) {
        const u32 size = (k < 16 ? p->vs : p->fs)->blocks[k & 15];
        if (!size) continue;
        VkDeviceSize at = 0;
        const VkBuffer b = buffer_read(gl.ubo[k].buffer, gl.ubo[k].offset + gl.ubo[k].size, &at);
        if (!b) {
            SAY(12, "[vk] a draw's uniform block %u has no buffer: left out\n", (u64)k);
            return 0;
        }
        ranges[nr] = (VkDescriptorBufferInfo){b, at + gl.ubo[k].offset, gl.ubo[k].size < size ? gl.ubo[k].size : size};
        if (!every && told.bound[k].a == (u64)b && told.bound[k].b == ranges[nr].offset && told.bound[k].c == ranges[nr].range) continue;
        told.bound[k].a = (u64)b, told.bound[k].b = ranges[nr].offset, told.bound[k].c = ranges[nr].range;
        writes[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = k, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                              .pBufferInfo = &ranges[nr++]};
    }
    for (u32 stage = 0; stage < 2; stage++)
        for (u32 k = 0; k < 8; k++) {
            if (!((stage ? p->fs : p->vs)->storage >> k & 1)) continue;
            VkDeviceSize at = 0;
            const VkBuffer b = buffer_read(gl.ssbo[stage * 8 + k].buffer, ~0u, &at);
            if (!b) return 0;
            ranges[nr] = (VkDescriptorBufferInfo){b, at + gl.ssbo[stage * 8 + k].offset, gl.ssbo[stage * 8 + k].size ? gl.ssbo[stage * 8 + k].size : VK_WHOLE_SIZE};
            writes[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 96 + stage * 8 + k, .descriptorCount = 1,
                                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ranges[nr++]};
        }
    for (u32 u = 0; u < p->units; u++) {
        const u32 k = p->unit_binding[u];
        if (!every && told.bound[k].a == (u64)images[u].sampler && told.bound[k].b == (u64)images[u].imageView && told.bound[k].c == (u64)images[u].imageLayout) continue;
        told.bound[k].a = (u64)images[u].sampler, told.bound[k].b = (u64)images[u].imageView, told.bound[k].c = (u64)images[u].imageLayout;
        writes[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = k, .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[u]};
    }
    if (nw) push_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, nw, writes);
    TICK(3);
    // the rest, said at each draw
    const float vy = gl.flip ? (float)(viewport.y + viewport.h) : (float)viewport.y, vh = gl.flip ? -(float)viewport.h : (float)viewport.h;
    const VkViewport v = {(float)viewport.x, vy, (float)viewport.w, vh, gl.near, gl.far};
    VkRect2D cut = {{0, 0}, {rendering_w, rendering_h}};
    if (gl.scissor_on) {
        int x0 = scissor.x < 0 ? 0 : scissor.x, y0 = scissor.y < 0 ? 0 : scissor.y, x1 = scissor.x + scissor.w, y1 = scissor.y + scissor.h;
        if (x1 > (int)rendering_w) x1 = (int)rendering_w;
        if (y1 > (int)rendering_h) y1 = (int)rendering_h;
        cut = (VkRect2D){{x0, y0}, {x1 > x0 ? (u32)(x1 - x0) : 0, y1 > y0 ? (u32)(y1 - y0) : 0}};
    }
    if (all || memcmp(&told.viewport, &v, sizeof v)) vkCmdSetViewport(cmd, 0, 1, &v), told.viewport = v;
    if (all || memcmp(&told.cut, &cut, sizeof cut)) vkCmdSetScissor(cmd, 0, 1, &cut), told.cut = cut;
    const u32 cull = !gl.cull_on ? VK_CULL_MODE_NONE : gl.cull_face == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : gl.cull_face == GL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK;
    if (all || told.cull != cull) vkCmdSetCullMode(cmd, cull), told.cull = cull;
    // (Vulkan reckons which way round a triangle goes with the sign the other way from OpenGL's, the coordinates being the same)
    if (all || told.front != gl.front_face) vkCmdSetFrontFace(cmd, gl.front_face == GL_CCW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE), told.front = gl.front_face;
    const u32 depth_test = gl.depth_test && t.depth_view, depth_write = gl.depth_test && gl.depth_write && t.depth_view, stencil_test = gl.stencil_test && t.stencil;
    if (all || told.depth_test != depth_test) vkCmdSetDepthTestEnable(cmd, depth_test), told.depth_test = depth_test;
    if (all || told.depth_write != depth_write) vkCmdSetDepthWriteEnable(cmd, depth_write), told.depth_write = depth_write;
    if (all || told.depth_func != gl.depth_func) vkCmdSetDepthCompareOp(cmd, compare_of(gl.depth_func)), told.depth_func = gl.depth_func;
    if (all || told.stencil_test != stencil_test) vkCmdSetStencilTestEnable(cmd, stencil_test), told.stencil_test = stencil_test;
    for (int k = 0; k < 2; k++) {
        const VkStencilFaceFlags face = k ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        if (!all && !memcmp(told.stencil[k], &gl.stencil[k], sizeof told.stencil[k])) continue;
        memcpy(told.stencil[k], &gl.stencil[k], sizeof told.stencil[k]);
        vkCmdSetStencilOp(cmd, face, stencil_of(gl.stencil[k].fail), stencil_of(gl.stencil[k].zpass), stencil_of(gl.stencil[k].zfail), compare_of(gl.stencil[k].func));
        vkCmdSetStencilCompareMask(cmd, face, gl.stencil[k].value_mask);
        vkCmdSetStencilWriteMask(cmd, face, gl.stencil[k].write_mask);
        vkCmdSetStencilReference(cmd, face, gl.stencil[k].ref);
    }
    if (all || told.offset_on != (u32)gl.offset_on) vkCmdSetDepthBiasEnable(cmd, gl.offset_on), told.offset_on = (u32)gl.offset_on;
    if (all || told.offset_units != gl.offset_units || told.offset_factor != gl.offset_factor)
        vkCmdSetDepthBias(cmd, gl.offset_units, 0, gl.offset_factor), told.offset_units = gl.offset_units, told.offset_factor = gl.offset_factor;
    if (all || memcmp(told.blend_color, gl.blend_color, 16)) vkCmdSetBlendConstants(cmd, gl.blend_color), memcpy(told.blend_color, gl.blend_color, 16);
    if (t.n && (all || told.ncolor != t.n || memcmp(told.blend, gl.blend, t.n * sizeof told.blend[0]))) {
        VkBool32 on[8];
        VkColorBlendEquationEXT equations[8];
        VkColorComponentFlags masks[8];
        for (u32 k = 0; k < t.n; k++) {
            on[k] = gl.blend[k].on;
            equations[k] = (VkColorBlendEquationEXT){factor_of(gl.blend[k].src), factor_of(gl.blend[k].dst), equation_of(gl.blend[k].eq), factor_of(gl.blend[k].srca),
                                                     factor_of(gl.blend[k].dsta), equation_of(gl.blend[k].eqa)};
            masks[k] = gl.blend[k].mask;
        }
        set_blend_enable(cmd, 0, t.n, on);
        set_blend_equation(cmd, 0, t.n, equations);
        set_write_mask(cmd, 0, t.n, masks);
        told.ncolor = t.n, memcpy(told.blend, gl.blend, t.n * sizeof told.blend[0]);
    }
    TICK(4);
    n_draws++;
    return 1;
}
void vk_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    if (draw_ready(mode)) vkCmdDraw(cmd, (u32)count, (u32)instances, (u32)first, 0);
    else n_left++;
}
static void draw_indexed(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances, GLint base) {
    VkDeviceSize at = 0;
    const VkBuffer b = buffer_read(gl.element_buffer, ~0u, &at);
    if (!b || !draw_ready(mode)) {
        n_left++;
        return;
    }
    u64 tick_ = armGetSystemTick();
    vkCmdBindIndexBuffer(cmd, b, at + (u64)indices, type == GL_UNSIGNED_BYTE ? VK_INDEX_TYPE_UINT8_EXT : type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, (u32)count, (u32)instances, 0, base, 0);
    TICK(5);
}
void vk_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) { draw_indexed(mode, count, type, indices, 1, 0); }
void vk_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances) { draw_indexed(mode, count, type, indices, instances, 0); }
void vk_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances, GLint base) {
    draw_indexed(mode, count, type, indices, instances, base);
}

// ---------------------------------------------------------------- a picture is shown
void glvk_show(void) {
    Tex *w = &texs[window_tex];
    u64 tick_ = armGetSystemTick();
    u32 index = 0;
    const VkResult got = vkAcquireNextImageKHR(device, swapchain, ~0ull, frames[frame].acquired, VK_NULL_HANDLE, &index);
    if (got < 0) failed("the next picture of the screen's", got);
    rendering_end();
    image_layout(w->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = chain_images[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    const VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)w->im->w, (int)w->im->h, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)chain_extent.width, (int)chain_extent.height, 1}}};
    vkCmdBlitImage(cmd, w->im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = 0, b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cmd);
    const VkPipelineStageFlags wait_at = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &frames[frame].acquired, .pWaitDstStageMask = &wait_at,
                             .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &frames[frame].drawn};
    vkResetFences(device, 1, &frames[frame].fence);
    CHECK("a picture's commands handed over", vkQueueSubmit(queue, 1, &si, frames[frame].fence));
    const VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &frames[frame].drawn, .swapchainCount = 1,
                                 .pSwapchains = &swapchain, .pImageIndices = &index};
    CHECK("a picture shown", vkQueuePresentKHR(queue, &pi));
    pipelines_keep();
    if (serial % 600 == 0) {
        nx2_log("[vk] picture %u: %u draws so far (%u left out), %u pipelines, %u blocks of buffers\n", (u64)serial, (u64)n_draws, (u64)n_left, (u64)npipes, (u64)nblocks);
        nx2_log("[vk]   a picture's draws, in us: textures %u, pipeline %u, vertices %u, descriptors %u, the rest said %u, the draw itself %u; showing %u\n",
                armTicksToNs(took[0] / 600) / 1000, armTicksToNs(took[1] / 600) / 1000, armTicksToNs(took[2] / 600) / 1000, armTicksToNs(took[3] / 600) / 1000,
                armTicksToNs(took[4] / 600) / 1000, armTicksToNs(took[5] / 600) / 1000, armTicksToNs(took[6] / 600) / 1000);
        memset(took, 0, sizeof took);
    }
    // the next picture: its turn's last commands are waited for, and what was let go at that turn is freed
    frame = (frame + 1) % FRAMES, serial++;
    vkWaitForFences(device, 1, &frames[frame].fence, VK_TRUE, ~0ull);
    retired_free(frame);
    pieces_settle();
    commands_begin();
    state_all_new();
    TICK(6);
    {
        // (No more than sixty pictures a second: a game counts its time in pictures, and the screen isn't waited for here. A
        // picture that took longer than a sixtieth isn't held to the screen's next turn either, as it would be if it were.)
        static u64 last;
        const u64 sixtieth = armNsToTicks(16666667ull), now = armGetSystemTick();
        if (last && now - last < sixtieth) svcSleepThread((s64)armTicksToNs(sixtieth - (now - last))), last += sixtieth;
        else last = now;
    }
}
// ---------------------------------------------------------------- what nvn2.c asks of OpenGL to compile a game's own GLSL
// (A game that carries GLSL text has no shaders in the port's file of SPIR-V: there is no compiler of GLSL here. Its programs
// don't link, which nvn2.c says in the log, and nothing is drawn with them.)
GLuint vk_glCreateShader(GLenum kind) { (void)kind; return 0; }
void vk_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *text, const GLint *length) { (void)shader, (void)count, (void)text, (void)length; }
void vk_glCompileShader(GLuint shader) { (void)shader; }
void vk_glGetShaderiv(GLuint shader, GLenum name, GLint *out) { (void)shader, (void)name; *out = 0; }
void vk_glGetShaderInfoLog(GLuint shader, GLsizei room, GLsizei *length, GLchar *out) { (void)shader; if (room > 0) snprintf(out, (size_t)room, "no GLSL compiler over Vulkan"); if (length) *length = 0; }
void vk_glDeleteShader(GLuint shader) { (void)shader; }
GLuint vk_glCreateProgram(void) { return 0; }
void vk_glAttachShader(GLuint program, GLuint shader) { (void)program, (void)shader; }
void vk_glLinkProgram(GLuint program) { (void)program; }
void vk_glGetProgramiv(GLuint program, GLenum name, GLint *out) { (void)program, (void)name; *out = 0; }
void vk_glGetProgramInfoLog(GLuint program, GLsizei room, GLsizei *length, GLchar *out) { (void)program; if (room > 0) snprintf(out, (size_t)room, "no GLSL compiler over Vulkan"); if (length) *length = 0; }
GLint vk_glGetUniformLocation(GLuint program, const GLchar *name) { (void)program, (void)name; return -1; }
void vk_glUniform1i(GLint location, GLint value) { (void)location, (void)value; }
void vk_glGetUniformIndices(GLuint program, GLsizei count, const GLchar *const *names, GLuint *out) { (void)program, (void)names; for (int k = 0; k < count; k++) out[k] = GL_INVALID_INDEX; }
void vk_glGetActiveUniformsiv(GLuint program, GLsizei count, const GLuint *indices, GLenum name, GLint *out) { (void)program, (void)indices, (void)name; for (int k = 0; k < count; k++) out[k] = 0; }
void vk_glGetActiveUniformBlockiv(GLuint program, GLuint block, GLenum name, GLint *out) { (void)program, (void)block, (void)name; *out = 0; }
void vk_glGetActiveUniformBlockName(GLuint program, GLuint block, GLsizei room, GLsizei *length, GLchar *out) { (void)program, (void)block; if (room > 0) out[0] = 0; if (length) *length = 0; }
void vk_glGetProgramInterfaceiv(GLuint program, GLenum interface, GLenum name, GLint *out) { (void)program, (void)interface, (void)name; *out = 0; }
void vk_glUniformBlockBinding(GLuint program, GLuint block, GLuint binding) { (void)program, (void)block, (void)binding; }
#endif
