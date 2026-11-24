// HDR output (MTF_HDR=1, read at start): an HDR10 swapchain (Rec. 2020 primaries, PQ) with the scene's highlights above
// the SDR white. Settings: MTF_HDR_PEAK_NITS (the display's peak), MTF_HDR_PAPER_WHITE (the picture's white) and
// MTF_HDR_UI_NITS (the HUD's and this menu's white), read again when the settings change. With MTF_HDR=0 nothing here
// runs and the present is what it was.
//
// MTF has no HDR output of its own: no shader of it encodes PQ or converts to Rec. 2020, and it calls none of the
// system's HDR functions. Its scene is HDR until one pass: the tonemap, which takes the light target (r11g11b10f) and
// the bloom, multiplies them by the exposure it measured, and maps them with ACES (Hill's fit) to 0..1 in an 8-bit sRGB
// target; the FXAA pass copies that into the window texture and the HUD is drawn over it. So:
// 1. vk_hdr_draw: the tonemap's draw (its fragment shader holds the fit's constants) gives its two textures and the
//    exposure (c5.x), noted with the frame.
// 2. vk_hdr_encode: on the present, shaders/hdr_compose.comp computes each pixel's SDR value again from those inputs;
//    where the window texture holds that value the pixel is the scene and gets the HDR value of the same inputs (the
//    game's curve up to 0.8, then a roll-off to the display's peak), anything else keeps its SDR value at the HUD's
//    white. A frame without the tonemap (menus, movies) is SDR at the HUD's white (shaders/hdr_encode.comp).
// 3. vk_hdr_ui_*: the in-game menu draws into an RGBA8 image and shaders/hdr_ui.frag blends it onto the swapchain.
// The swapchain, metadata, encode and menu passes contain adapted code under the MIT licence: authors and licence texts
// in THIRD_PARTY_NOTICES.md.
#include "vk.h"

#include <math.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hdr_compose_spv.h"
#include "hdr_encode_spv.h"
#include "hdr_fullscreen_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "platform/platform.h"

#define SLOTS 16

// ---- settings

static int g_wanted = -1;

int vk_hdr_wanted(void) {
    if (g_wanted < 0) g_wanted = cfg_bool("MTF_HDR");
    return g_wanted;
}

void vk_hdr_off(void) { g_wanted = 0; }

// The three nits, read again when the settings change. The replay and the present thread read them: an update fills
// the snapshot not in use and then publishes it.
typedef struct { float peak, game, ui; } Nits;
static struct { unsigned seen; int cur; Nits snap[2]; PlatMutex lock; } N = {.seen = ~0u, .lock = PLAT_MUTEX_INIT};

static Nits nits(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&N.seen, __ATOMIC_ACQUIRE) != v) {
        plat_mutex_lock(&N.lock);
        if (N.seen != v) {
            Nits *n = &N.snap[!N.cur];
            n->peak = fmaxf((float)cfg_double("MTF_HDR_PEAK_NITS"), 80.0f);
            n->game = fmaxf((float)cfg_double("MTF_HDR_PAPER_WHITE"), 80.0f);
            n->ui = fmaxf((float)cfg_double("MTF_HDR_UI_NITS"), 80.0f);
            __atomic_store_n(&N.cur, !N.cur, __ATOMIC_RELEASE);
            __atomic_store_n(&N.seen, v, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&N.lock);
    }
    return N.snap[__atomic_load_n(&N.cur, __ATOMIC_ACQUIRE)];
}

// The support buffer's HDR words (read only by a replaced tonemap shader, which MTF does not have): the picture's white
// against the HUD's, the peak against the picture's white.
void vk_hdr_support(float out[4]) {
    Nits n = nits();
    out[0] = n.game / n.ui;
    out[1] = n.peak > n.game ? n.peak / n.game : 1.0f;
    out[2] = out[3] = 0;
}

// The game's shaders run as they are: the HDR picture is made on the present (vk_hdr_encode).
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) { (void)spirv, (void)words; }

// ---- 1. the tonemap's inputs

int vk_hdr_track;                   // vk_exec_draw.c calls vk_hdr_draw: set at the first present with MTF_HDR=1
static volatile int32_t g_active;   // the swapchain is HDR10 (gfx_present.c vk_hdr_set_active)

void vk_hdr_set_active(int on) { plat_xchg32(&g_active, on); }
int vk_hdr_active(void) { return plat_cas32(&g_active, 0, 0); }

enum { SPV_OP_TYPE_FLOAT = 22, SPV_OP_CONSTANT = 43 };

// 1 when the shader holds the ACES fit's constants: its input matrix (0.59719), the fit (0.983729, 0.4329510) and its
// output matrix (1.60475): the tonemap, whatever its hash.
static int has_aces(const GfxShader *s) {
    uint64_t n = 0;
    const uint32_t *w = gfx_shader_spirv(s, &n);
    static const float want[4] = {0.597190022f, 0.983729005f, 0.432951003f, 1.60475004f};
    uint32_t float_type = 0, found = 0;
    for (uint64_t i = 5; w && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4 && a[0] == float_type && float_type)
            for (int k = 0; k < 4; k++) {
                float f;
                memcpy(&f, &a[2], 4);
                if (f == want[k]) found |= 1u << k;
            }
        i += len;
    }
    return found == 15;
}

static int is_tonemap(const GfxShader *fs) {
    static struct { const GfxShader *s; int yes; } memo[256];
    uintptr_t h = ((uintptr_t)fs >> 4) * 0x9E3779B97F4A7C15ull >> 56;
    for (uint32_t i = 0, j = (uint32_t)h; i < 256; i++, j = (j + 1) & 255) {
        if (memo[j].s == fs) return memo[j].yes;
        if (!memo[j].s) {
            memo[j].yes = has_aces(fs);
            memo[j].s = fs;
            if (memo[j].yes) rt_log("hdr: the game's tonemap found (its ACES constants): the scene goes out in HDR");
            return memo[j].yes;
        }
    }
    return 0;
}

// The last tonemap draw's inputs: the light target and the bloom, the exposure, the frame. Replay thread writes, the
// present reads (under T.lock).
static struct {
    PlatMutex lock;
    VkImage light, bloom;
    VkFormat lfmt, bfmt;
    uint32_t lw, lh;
    float exposure;
    uint64_t frame;
} T = {.lock = PLAT_MUTEX_INIT};

// Whether the exposure can be read. Every draw of a shader that carries the tonemap asks (22 such shaders in a match),
// and an OS query per draw took 0.4 of a core: the last page that passed answers until a pool is mapped or goes away.
static int exposure_readable(uint64_t a) {
    static uint64_t page, gen;   // replay thread
    uint64_t p = a >> 12, g = vk_mem_gen();
    if (p == page && g == gen && (a & 0xfff) <= 0xffc) return 1;
    if (!mem_guest_readable(a, 4)) return 0;
    page = p, gen = g;
    return 1;
}

void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info, const GfxShader *fs) {
    if (!fs || !is_tonemap(fs)) return;
    const VkTexImage *tex[2] = {NULL, NULL};
    int ntex = 0;
    float exposure = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->stage != GFX_STAGE_FS) continue;
        if (bd->kind == GFX_RES_TEX && ntex < 2 && bd->res->slot >= 0 && bd->res->slot < 128 && ex->tex[1][bd->res->slot])
            tex[ntex++] = vk_texture_by_id(NVN_HANDLE_TEX(ex->tex[1][bd->res->slot]), NULL, NULL);
        if (bd->kind == GFX_RES_UBO && bd->res->slot == 5) {
            const p_bind_buf *u = &ex->ubo[1][5 - 3];
            if (u->addr && exposure_readable(u->addr)) memcpy(&exposure, (const void *)(uintptr_t)u->addr, 4);
        }
    }
    if (ntex != 2 || !tex[0] || !tex[1] || !(exposure > 0) || !isfinite(exposure)) return;
    if (tex[0]->gone || tex[1]->gone) return;   // dropped since the lookup: kept by handle, it would outlive the image
    // the light target is the larger one, the size of the scene; the bloom is sampled at the same coordinates
    if ((uint64_t)tex[1]->width * tex[1]->height > (uint64_t)tex[0]->width * tex[0]->height) {
        const VkTexImage *t = tex[0];
        tex[0] = tex[1], tex[1] = t;
    }
    plat_mutex_lock(&T.lock);
    T.light = tex[0]->image, T.lfmt = tex[0]->format, T.lw = tex[0]->width, T.lh = tex[0]->height;
    T.bloom = tex[1]->image, T.bfmt = tex[1]->format;
    T.exposure = exposure;
    T.frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    plat_mutex_unlock(&T.lock);
}

void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) {
    (void)win, (void)image, (void)format;   // the present reads the window texture itself (vk_hdr_encode)
    if (vk_hdr_wanted()) vk_hdr_track = 1;
}

void vk_hdr_image_gone(VkImage image) {   // a texture image went away: forget it as the tonemap's input
    if (!vk_hdr_track) return;
    plat_mutex_lock(&T.lock);
    if (T.light == image || T.bloom == image) T.light = T.bloom = VK_NULL_HANDLE;
    plat_mutex_unlock(&T.lock);
}

// ---- swapchain

// With MTF_HDR=1, the HDR10 format among the surface's (10-bit UNORM, ST 2084): 1 with *out set, else 0 and the
// reason logged once.
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted()) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        rt_log("hdr: surface formats (VkFormat/VkColorSpaceKHR):%s", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) rt_log("hdr: HDR10 swapchain (format %d)", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged)
        rt_log("hdr: the display offers no HDR10 format, so the picture stays SDR (Windows: turn on \"Use HDR\" in the "
               "display settings)");
    logged = 1;
    return 0;
}

// The display's peak and the picture's white for sc; again when the settings change (gfx_present.c calls it on every
// present to an HDR10 swapchain, with new_sc = 1 for a new swapchain).
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) {
    static unsigned sent = ~0u;
    VkCtx *c = vk_ctx();
    if (!c->has_hdr_metadata || (!new_sc && cfg_version() == sent)) return;
    sent = cfg_version();
    PFN_vkSetHdrMetadataEXT set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    Nits n = nits();
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = n.peak, .minLuminance = 0.0f, .maxContentLightLevel = n.peak,
                          .maxFrameAverageLightLevel = n.game};
    set(c->device, 1, &sc, &m);
}

// ---- present passes

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

typedef struct { VkImage img; VmaAllocation alloc; VkImageView view; uint32_t w, h; } Img;

static void img_free(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    *m = (Img){0};
}

// m at w x h in fmt; another size waits for the device first (earlier presents may still read the old image).
static int img_ensure(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    if (m->img) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, m);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        m->view = VK_NULL_HANDLE;
        img_free(c, m);
        return 0;
    }
    m->w = w, m->h = h;
    return 1;
}

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampler, linear;
    VkDescriptorSetLayout dsl_enc, dsl_comp, dsl_ui;
    VkPipelineLayout enc_layout, comp_layout, ui_layout;
    VkPipeline enc, comp;
    VkPipeline ui;                  // for ui_fmt
    VkFormat ui_fmt;
    Img out;                        // PQ code values of the crop, RGBA16F
    Img menu;                       // the in-game menu, RGBA8
    VkImageView views[SLOTS][3];    // per present slot: the window texture, the light target, the bloom
    float vk, yk, slope;            // the curve's knee (hdr_compose.comp)
} H;

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkDescriptorSetLayout set_layout(VkCtx *c, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(c->device, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipelineLayout pipe_layout(VkCtx *c, VkDescriptorSetLayout *dsl, VkShaderStageFlags st, uint32_t pc) {
    VkPushConstantRange pr = {st, 0, pc};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(c->device, &li, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipeline compute(VkCtx *c, const uint32_t *code, size_t size, VkPipelineLayout layout) {
    VkShaderModule m = module(c, code, size);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (m && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

// The ACES fit (hdr_compose.comp aces_fit) and the knee where the HDR curve leaves it: the input v_k whose output is
// y_k (a root of the fit's quadratic), and the fit's slope there.
static double aces_fit(double v) { return (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081); }

static void knee(float yk) {
    double a = 1.0 - yk * 0.983729, b = 0.0245786 - yk * 0.4329510, cc = -(0.000090537 + yk * 0.238081);
    double v = (-b + sqrt(b * b - 4.0 * a * cc)) / (2.0 * a), e = 1e-4 * v;
    H.vk = (float)v, H.yk = yk;
    H.slope = (float)((aces_fit(v + e) - aces_fit(v - e)) / (2.0 * e));
}

static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    const VkShaderStageFlags CS = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding enc[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = CS}};
    VkDescriptorSetLayoutBinding comp[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = CS},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = CS}};
    VkDescriptorSetLayoutBinding ui = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSamplerCreateInfo li = si;
    li.magFilter = li.minFilter = VK_FILTER_LINEAR;   // the bloom, smaller than the scene, as the tonemap filters it
    if (!H.push || vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        vkCreateSampler(c->device, &li, NULL, &H.linear) != VK_SUCCESS || !(H.dsl_enc = set_layout(c, enc, 2)) ||
        !(H.dsl_comp = set_layout(c, comp, 4)) || !(H.dsl_ui = set_layout(c, &ui, 1)) ||
        !(H.enc_layout = pipe_layout(c, &H.dsl_enc, CS, 32)) || !(H.comp_layout = pipe_layout(c, &H.dsl_comp, CS, 48)) ||
        !(H.ui_layout = pipe_layout(c, &H.dsl_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 16)) ||
        !(H.enc = compute(c, hdr_encode_spv, sizeof hdr_encode_spv, H.enc_layout)) ||
        !(H.comp = compute(c, hdr_compose_spv, sizeof hdr_compose_spv, H.comp_layout))) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    knee(0.97f);
    rt_log("hdr: the scene's curve leaves ACES at %.3f (input %.3f, slope %.3f)", H.yk, H.vk, H.slope);
    return H.ok = 1;
}

static VkImageView view_of(VkCtx *c, VkImageView *keep, VkImage img, VkFormat fmt) {
    if (*keep) vkDestroyImageView(c->device, *keep, NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, keep) != VK_SUCCESS) *keep = VK_NULL_HANDLE;
    return *keep;
}

static VkFormat unorm_of(VkFormat f) {   // the window texture's sRGB codes, read as they are (images are MUTABLE_FORMAT)
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.out, (uint32_t)crop[2], (uint32_t)crop[3], VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    slot %= SLOTS;
    VkFormat raw = unorm_of(fmt);
    int codes = raw == VK_FORMAT_R8G8B8A8_UNORM || raw == VK_FORMAT_B8G8R8A8_UNORM;   // 8-bit sRGB codes
    if (!view_of(c, &H.views[slot][0], src, raw)) return VK_NULL_HANDLE;
    // the scene: this frame's or the last one's tonemap inputs, of the crop's size
    plat_mutex_lock(&T.lock);
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int scene = codes && T.light && T.bloom && T.lw >= (uint32_t)(crop[2] / 2) && T.lh >= (uint32_t)(crop[3] / 2) &&
                now - T.frame <= 2;
    VkImage light = T.light, bloom = T.bloom;
    VkFormat lfmt = T.lfmt, bfmt = T.bfmt;
    float exposure = T.exposure;
    plat_mutex_unlock(&T.lock);
    if (scene && (!view_of(c, &H.views[slot][1], light, lfmt) || !view_of(c, &H.views[slot][2], bloom, bfmt))) scene = 0;
    static int said = -1;
    if (scene != said) said = scene, rt_log("hdr: %s", scene ? "the scene in HDR, the HUD at its white"
                                                               : "no scene this frame: SDR at the HUD's white");
    Nits n = nits();
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkImage reads[3] = {src, scene ? light : VK_NULL_HANDLE, scene ? bloom : VK_NULL_HANDLE};
    for (int i = 0; i < 3; i++)
        if (reads[i])
            barrier(cb, reads[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL};
    if (scene) {
        VkDescriptorImageInfo ii[3] = {{H.sampler, H.views[slot][0], VK_IMAGE_LAYOUT_GENERAL},
                                       {H.sampler, H.views[slot][1], VK_IMAGE_LAYOUT_GENERAL},
                                       {H.linear, H.views[slot][2], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[4];
        for (int i = 0; i < 4; i++)
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                          .descriptorCount = 1,
                                          .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                  : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo = i < 3 ? &ii[i] : &oi};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.comp);
        rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.comp_layout, 0, 4, w);
        struct { int32_t offset[2], size[2]; float n[4], k[4]; } pc = {
            {crop[0], crop[1]}, {crop[2], crop[3]}, {n.ui, n.game, n.peak, exposure},
            {H.vk, H.yk, H.slope, fminf(fmaxf(n.peak / n.game, H.yk + 0.01f), 2.0f)}};
        vkCmdPushConstants(cb, H.comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    } else {
        VkDescriptorImageInfo ii = {H.sampler, H.views[slot][0], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc);
        rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc_layout, 0, 2, w);
        struct { int32_t offset[2], size[2]; float p[4]; } pc = {{crop[0], crop[1]}, {crop[2], crop[3]},
                                                                {n.ui, n.peak, codes ? 1.0f : 0.0f, 0}};
        vkCmdPushConstants(cb, H.enc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    }
    vkCmdDispatch(cb, ((uint32_t)crop[2] + 7) / 8, ((uint32_t)crop[3] + 7) / 8, 1);
    for (int i = 0; i < 3; i++)
        if (reads[i]) barrier(cb, reads[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    return H.out.img;
}

// The in-game menu's target: an RGBA8 image of the swapchain's size in COLOR_ATTACHMENT_OPTIMAL (its old content
// dropped); the caller renders the menu into the view with a clear to 0.
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.menu, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return H.menu.view;
}

static int ui_pipeline(VkCtx *c, VkFormat fmt) {
    if (H.ui && H.ui_fmt == fmt) return 1;
    if (H.ui) {   // another swapchain format: earlier presents may still use the old pipeline
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vkDestroyPipeline(c->device, H.ui, NULL);
        H.ui = VK_NULL_HANDLE;
    }
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv),
                   fs = module(c, hdr_ui_spv, sizeof hdr_ui_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = H.ui_layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &H.ui) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        H.ui = VK_NULL_HANDLE;
        rt_log("hdr: the menu pass could not be created; the menu is not shown");
        return 0;
    }
    H.ui_fmt = fmt;
    return 1;
}

// The menu image onto dst (a swapchain image in COLOR_ATTACHMENT_OPTIMAL, view dst_view of format fmt).
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.menu.img || !ui_pipeline(c, fmt)) return;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D s = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    VkDescriptorImageInfo ii = {H.sampler, H.menu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui_layout, 0, 1, &wr);
    float pc[4] = {nits().ui, 0, 0, 0};
    vkCmdPushConstants(cb, H.ui_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < SLOTS; i++)
        for (int k = 0; k < 3; k++)
            if (H.views[i][k]) vkDestroyImageView(c->device, H.views[i][k], NULL);
    img_free(c, &H.out);
    img_free(c, &H.menu);
    if (H.enc) vkDestroyPipeline(c->device, H.enc, NULL);
    if (H.comp) vkDestroyPipeline(c->device, H.comp, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.enc_layout) vkDestroyPipelineLayout(c->device, H.enc_layout, NULL);
    if (H.comp_layout) vkDestroyPipelineLayout(c->device, H.comp_layout, NULL);
    if (H.ui_layout) vkDestroyPipelineLayout(c->device, H.ui_layout, NULL);
    if (H.dsl_enc) vkDestroyDescriptorSetLayout(c->device, H.dsl_enc, NULL);
    if (H.dsl_comp) vkDestroyDescriptorSetLayout(c->device, H.dsl_comp, NULL);
    if (H.dsl_ui) vkDestroyDescriptorSetLayout(c->device, H.dsl_ui, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    if (H.linear) vkDestroySampler(c->device, H.linear, NULL);
    memset(&H, 0, sizeof H);
}

// PQ code (0..1023) of a 10-bit channel to the sRGB byte of the same colour at the HUD's white (menu test shots and
// flight thumbnails of an HDR10 swapchain).
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    float ui = nits().ui;
    // Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix)
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};   // A2B10G10R10: R low
        if (a2r) {
            float t = c[0];
            c[0] = c[2], c[2] = t;
        }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
            out[4 * i + k] = (uint8_t)(s * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}
