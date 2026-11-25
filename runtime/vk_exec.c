// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop (END / BEGIN / JUMP / CALL, which ops are pure state, the state defaults, render targets kept until they
// change, clears skipped while rendering is disabled) and the pass begin and end (GENERAL-layout attachments,
// LOAD/STORE, render area from the smallest attachment, a full barrier before each pass), the scissor clip and
// attachment clears are adapted code.
// Contains adapted code under the MIT licence: authors and licence texts in THIRD_PARTY_NOTICES.md.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
static _Thread_local int x_here;   // this thread is inside vk_exec_record (vk_exec_inline_begin)

// Parallel passes (MTF_REPLAY_THREADS): state shared with pass handlers.
#define PAR_MIN_DRAWS 256
#define PAR_SLICE_DRAWS 1024
#define PAR_SLICE_MIN 48
#define PAR_EXPECT 1024
#define PAR_HIST 256
#define PAR_BAN_AFTER 8
#define PAR_MAX_SLICES 1024
#define PAR_MAX_OPS (1 << 20)

typedef struct { uint64_t sig; uint32_t draws; uint8_t banned; } ParHist;
static ParHist par_hist[PAR_HIST];
static int par_mode = -1;
static volatile uint8_t pdone[PAR_MAX_SLICES];
static uint64_t par_usd[VK_EXEC_UPD_SNAPS];
static uint64_t par_t0, par_pass_seq;
static uint64_t par_t_collect, par_t_own, par_t_wait, par_t_exec, par_wait_max, par_wait_long;
static volatile int64_t par_rec[2][3], par_slow[2], par_slow_ns, par_slow_draws;
static uint64_t par_restarts, par_passes, par_slices, par_sliced_draws, par_skipped_oq, par_no[5];
static _Thread_local int par_slice, par_hazard;
static int par_restart, pass_secondary, pass_tried, pass_sliced;
static uint32_t pass_draws;

static struct {
    int on;
    uint64_t sig;
    const op_hdr **ops;
    int nops;
    VkExec *snap;
    int start[PAR_MAX_SLICES], end[PAR_MAX_SLICES];
    int nslices, s0, upd;
    uint8_t soq[PAR_MAX_SLICES], supd[PAR_MAX_SLICES];
    VkQueryControlFlags soqf[PAR_MAX_SLICES];
    int draws, slice_draws, slice_size, expect, oq;
    VkQueryControlFlags oq_flags;
} PC;

typedef struct {
    VkCommandPool pool;
    PlatMutex lock;
    VkCommandBuffer *free;
    int nfree, cfree;
} ParPool;
typedef struct { ParPool *pp; VkCommandBuffer cb; } ParCb;
typedef struct { int n; ParCb cb[]; } ParDone;

static void *par_grow(void *p, int *cap, size_t size) {
    int c = *cap ? *cap * 2 : 64;
    void *q = realloc(p, (size_t)c * size);
    if (!q) rt_fatal("vk: out of memory");
    *cap = c;
    return q;
}

static uint64_t par_sig(const VkExec *ex) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        uint64_t v[2] = {0, 0};
        if ((i == 8 || i < ex->pass_ncolor) && t->image)
            v[0] = (uint64_t)(uintptr_t)t->image, v[1] = (uint64_t)t->level << 40 | (uint64_t)t->base_layer << 20 | t->layers;
        h = (h ^ v[0]) * 0x100000001b3ull;
        h = (h ^ v[1]) * 0x100000001b3ull;
    }
    return h | 1;
}

static uint32_t par_hist_slot(uint64_t sig) { return (uint32_t)((sig ^ sig >> 29) * 0x9E3779B97F4A7C15ull >> 56) % PAR_HIST; }

static struct { uint64_t key; uint32_t draws; } par_exp[PAR_EXPECT];
static struct { uint64_t sig; uint32_t n; } par_piece[64];
static int npiece;

static uint64_t par_piece_key(uint64_t sig, int next) {
    int i = 0;
    while (i < npiece && par_piece[i].sig != sig) i++;
    if (i == npiece) {
        if (npiece == 64) return sig;
        par_piece[npiece++] = (typeof(par_piece[0])){sig, 0};
    }
    uint64_t key = (sig ^ (uint64_t)par_piece[i].n * 0x9E3779B97F4A7C15ull) | 1;
    if (next) par_piece[i].n++;
    return key;
}

static uint32_t *par_expect(uint64_t key, int add) {
    typeof(par_exp[0]) *x = &par_exp[(key * 0x9E3779B97F4A7C15ull) >> 54];
    if (x->key != key) {
        if (!add) return NULL;
        x->key = key, x->draws = 0;
    }
    return &x->draws;
}

static ParHist *par_find(uint64_t sig, int add) {
    uint32_t i0 = par_hist_slot(sig);
    ParHist *low = NULL;
    for (uint32_t k = 0; k < 4; k++) {
        ParHist *e = &par_hist[(i0 + k) % PAR_HIST];
        if (e->sig == sig) return e;
        if (!low || (!e->sig && low->sig) || (e->sig && low->sig && e->draws < low->draws)) low = e;
    }
    if (!add) return NULL;
    *low = (ParHist){sig, 0, 0};
    return low;
}

// ---- MTF_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_here || par_slice ? X.fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu; targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_ub_flush(VkExec *ex) { (void)ex; }
WEAK void vk_exec_ub_foreign(VkExec *ex, uint64_t addr, uint64_t n) { (void)ex, (void)addr, (void)n; }
WEAK void vk_exec_ub_perf(void) {}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

// NVN defaults of the replay state.
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, MTF_BLOCK_VIEWS_3D): on RDNA2 cards such an image
// made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took GPU writes past the end of its memory (issue #5). Draws
// into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64 volumes and their
// levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer per
// depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each pass
// end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    if (img->gone) return 0;   // dropped since the lookup: a stand-in kept by its handle would outlive it
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here: left out",
                  t->d.format, t->d.target, t->d.width, t->d.height);
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->scale = img->scale ? img->scale : 1;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384, as
   NVN sizes it. A fragment shader that only writes a storage buffer may draw into a 1x1 viewport; a pass sized by the
   viewport's width alone puts an offset viewport outside the scissor, and the draw is lost. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, scale = 1;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
        if (t->scale > scale) scale = t->scale;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
        if (d->scale > scale) scale = d->scale;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_scale = ex->pass_noatt ? 1 : scale;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history
// The presented frame (gfx_counters.frames + 1; 0: never) each target subresource was last rendered into, and the one
// before. prepare_draw lets a draw into a target the previous frame did not render (a one-shot draw: a volume or LUT
// generated once) wait for its shaders and pipeline instead of skipping it. Per subresource, not per image: a volume
// may be generated a few slices per frame. Direct-mapped: a slot another subresource took reads as not rendered, so a
// collision makes a draw wait rather than skip.
typedef struct { VkImage image; uint32_t level, layer; uint64_t last, prev; } TargetFrames;
static TargetFrames tfr[1024];

static TargetFrames *tfr_slot(const VkExecTarget *t) {
    uint64_t h = ((uint64_t)(uintptr_t)t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->base_layer << 40) *
                 0x9E3779B97F4A7C15ull;
    return &tfr[h >> 54];
}

static int tfr_match(const TargetFrames *e, const VkExecTarget *t) {
    return e->image == t->image && e->level == t->level && e->layer == t->base_layer;
}

static void tfr_note(const VkExecTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_slot(t);
    if (!tfr_match(e, t)) *e = (TargetFrames){t->image, t->level, t->base_layer, 0, 0};
    if (e->last != stamp) e->prev = e->last, e->last = stamp;
}

static int tfr_recent(const VkExecTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_slot(t);
    return tfr_match(e, t) && (e->last == stamp ? e->prev : e->last) == stamp - 1;
}

// No frame before the current one rendered the target (a slot another subresource took reads the same).
static int tfr_first(const VkExecTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_slot(t);
    return !tfr_match(e, t) || !(e->last == stamp ? e->prev : e->last);
}

// With any, a recent depth target alone does not make the pass per-frame when a colour target is rendered for the
// first time: the game's item icon capture (an inventory or Fuse icon) draws into a new colour target with the
// per-frame depth buffer, once. Its draws skipped there stay missing from the icon until the item is captured again.
int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    int n = 0, color = 0, depth = 0, color_first = 0;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        n++;
        if (tfr_recent(t, stamp)) *(i < 8 ? &color : &depth) = 1;
        else if (!any) return 0;
        else if (i < 8 && tfr_first(t, stamp)) color_first = 1;
    }
    return any ? color || (depth && !color_first) : n > 0;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        const TargetFrames *e = tfr_slot(t);
        int m = tfr_match(e, t);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu", n ? "; " : "", i < 8 ? 'c' : 'd',
                         i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(m ? e->last : 0), (unsigned long long)(m ? e->prev : 0));
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    vk_exec_pass_end(ex);
    vk_exec_ub_flush(ex);   // uniform updates overlaid in the pass reach their buffers before the barrier below
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) {
    if (par_slice) { if (!par_hazard) par_hazard = 1; return; }
    barrier_to(ex, NULL, NULL, 0);
}

void vk_exec_pass_begin(VkExec *ex) {
    if (par_slice) { if (!par_hazard) par_hazard = 1; return; }
    if (ex->in_pass) return;
    resolve_rt(ex);
    vk_contact_pass_begin(ex);
    vk_dlaa_pass(ex);   // DLAA runs before the bloom chain's first pass (vk_dlaa.c)
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to(ex, want, wasp, nwant);   // passes, clears, copies and dispatches see each other's results
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    // The start mark goes between the barrier and vkCmdBeginRendering, its end right after vkCmdEndRendering
    // (vk_exec_pass_end): outside the pass, and without breaking it.
    VkPassMark *m = ex->mark = vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence);
    if (m) {
        m->step = ex->mark_passes++;
        m->w = ex->pass_w, m->h = ex->pass_h, m->d = ex->pass_layers;
        m->ncolor = (uint8_t)(ex->pass_ncolor < 8 ? ex->pass_ncolor : 8);
        for (int i = 0; i < m->ncolor; i++) m->color[i] = ex->pass_color[i];
        m->depth = ex->pass_depth ? ex->pass_depth : ex->pass_stencil;
        // The shaders bound now are the first draw's when it repeats the pipeline state; when it does not,
        // vk_exec_draw.c prepare_draw replaces them as it records the new state.
        vk_exec_draw_shaders(&m->first[0], &m->first[1]);
        ex->mark_draws0 = vk_exec_frame.draws;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .flags = pass_secondary ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0,
                          .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
    pass_tried = 0, pass_sliced = pass_secondary, pass_draws = 0;
}

void vk_exec_pass_end(VkExec *ex) {
    if (par_slice) { if (!par_hazard) par_hazard = 1; return; }
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    vk_exec_oq_pass_ended(ex);
    ex->recorded++;
    ex->in_pass = 0;
    if (!par_restart && ex == &X && pass_draws) {
        uint64_t sig = par_sig(ex);
        if (pass_draws >= PAR_MIN_DRAWS) {
            ParHist *e = par_find(sig, 1);
            if (pass_draws > e->draws) e->draws = pass_draws;
        }
        uint32_t *xp = par_expect(par_piece_key(sig, 1), 1);
        if (xp) *xp = pass_draws;
    }
    if (ex->mark) {
        // vk_exec_frame_log resets the frame counters at a present (MTF_TRACE=gfx): then only the draws since it count
        uint64_t d = vk_exec_frame.draws, d0 = ex->mark_draws0;
        ex->mark->draws = (uint32_t)(d >= d0 ? d - d0 : d);
        if (ex->mark->draws) vk_exec_draw_shaders(&ex->mark->last[0], &ex->mark->last[1]);
        vk_pass_mark_end(ex->cb, ex->mark);
        ex->mark = NULL;
    }
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex), tfr_note(&ex->color[i], stamp);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = &X;
    *reopen = 0;
    if (!x_here || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    X.recorded++;
    if (reopen) vk_exec_pass_begin(&X);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(X.fence, free_inline_buf, t);
    return t->b;
}

static VkRect2D scissor_rect(const VkExec *ex, const p_scissor *s) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!s) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t k = ex->pass_scale > 1 ? ex->pass_scale : 1;
    int64_t x0 = s->x * k, y0 = s->y * k;
    int64_t x1 = x0 + (int64_t)s->w * k, y1 = y0 + (int64_t)s->h * k;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

VkRect2D vk_exec_scissor(const VkExec *ex) { return scissor_rect(ex, ex->has_scissor ? &ex->scissor : NULL); }

VkRect2D vk_exec_scissor_at(const VkExec *ex, uint32_t i) {
    if (!i) return vk_exec_scissor(ex);
    return scissor_rect(ex, i < 16 && ex->scissor_mask >> i & 1 ? &ex->scissor_n[i] : NULL);
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    if (ex->render_disabled) return;
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        static int full = -1;   // MTF_STENCIL_MASK_FULL=1 (diagnosis): a masked stencil clear clears every bit
        if (full < 0) full = cfg_bool("MTF_STENCIL_MASK_FULL");
        if (sm == 0xff || full) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once MTF_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value.
// A part also spans at least MTF_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
static int split_n = -1, split_draws, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs;

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("MTF_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("MTF_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        rt_log("vk: replay command buffer split every %d draws between passes, parts of at least %d us%s", split_n,
               us > 0 ? us : 0, split_n ? "" : " (off)");
    }
    if (!split_n || ex->in_pass || split_draws < split_n) return;
    if (split_ns && kern_now_ns() - part_t0 < split_ns) return;   // split_draws stays: the next draw asks again
    split_draws = 0;
    if (ex->recorded == part_start) return;
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

static void state_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++)
            if (v->first + i >= 0) {
                memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
                if ((uint32_t)(v->first + i) >= ex->vp_n) ex->vp_n = (uint32_t)(v->first + i) + 1;
            }
        ex->has_vp = 1;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_SCISSOR: {
        const p_scissor *s = a;
        if (!s->index) ex->scissor = *s, ex->has_scissor = 1;
        else if (s->index > 0 && s->index < 16) ex->scissor_n[s->index] = *s, ex->scissor_mask |= 1u << s->index;
        ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
        break;
    }
    case OP_DEPTH_RANGE: {
        const p_depth_range *r = a;
        ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_STENCIL: {
        const p_stencil *s = a;
        for (int f = 0; f < 2; f++) {
            if (!(s->face & (1 << f))) continue;
            if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
            else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
            else ex->st_ref[f] = (uint32_t)s->ref;
        }
        ex->dirty |= VK_EXEC_DIRTY_STENCIL;
        break;
    }
    case OP_POLY_OFFSET:
        ex->poly_offset = *(const p_poly_offset *)a;
        ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
        break;
    case OP_BLEND_COLOR:
        memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
        ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
        break;
    case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
    case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
    case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
    case OP_PATCH_SIZE: ex->patch_size = ((const p_flag *)a)->value; break;
    case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_COPY_IMAGE_STRIDE: ex->copy_image_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
    case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
    }
}

static void par_done(void *arg) {
    ParDone *d = arg;
    for (int i = 0; i < d->n; i++) {
        ParPool *pp = d->cb[i].pp;
        plat_mutex_lock(&pp->lock);
        if (pp->nfree == pp->cfree) pp->free = par_grow(pp->free, &pp->cfree, sizeof *pp->free);
        pp->free[pp->nfree++] = d->cb[i].cb;
        plat_mutex_unlock(&pp->lock);
    }
    free(d);
}

static VkCommandBuffer par_secondary(ParPool *pp) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = VK_NULL_HANDLE;
    plat_mutex_lock(&pp->lock);
    if (pp->nfree) cb = pp->free[--pp->nfree];
    plat_mutex_unlock(&pp->lock);
    if (cb) {
        vkResetCommandBuffer(cb, 0);
        return cb;
    }
    if (!pp->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &pp->pool) != VK_SUCCESS) rt_fatal("vk: secondary command pool failed");
    }
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pp->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &cb) != VK_SUCCESS) rt_fatal("vk: secondary command buffer failed");
    return cb;
}

static void par_begin_secondary(const VkExec *ex, VkCommandBuffer cb, int oq, VkQueryControlFlags oq_flags) {
    VkFormat color[8];
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) color[i] = ex->color[i].image ? ex->pass_color[i] : VK_FORMAT_UNDEFINED;
    const VkExecTarget *d = &ex->depth;
    VkCommandBufferInheritanceRenderingInfo ir = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .colorAttachmentCount = ex->pass_ncolor, .pColorAttachmentFormats = color,
        .depthAttachmentFormat = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? ex->pass_depth : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? ex->pass_stencil : VK_FORMAT_UNDEFINED,
        .rasterizationSamples = (VkSampleCountFlagBits)(ex->pass_samples ? ex->pass_samples : 1)};
    VkCommandBufferInheritanceInfo ii = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &ir,
                                         .occlusionQueryEnable = oq ? VK_TRUE : VK_FALSE,
                                         .queryFlags = oq ? oq_flags : 0};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                            VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                   .pInheritanceInfo = &ii};
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) rt_fatal("vk: secondary command buffer begin failed");
}

#define PAR_MAX_WORKERS 15
typedef struct {
    PlatThread *t;
    int k;
    ParPool pp;
    VkExec w;
} ParWorker;
static ParWorker *par_workers[PAR_MAX_WORKERS];
static int par_nworkers;

static PlatMutex pw_lock = PLAT_MUTEX_INIT;
static PlatCond pw_go, pw_idle;
static int pw_conds, pw_pub, pw_next;
static volatile int pw_active;
static ParPool par_main_pool = {.lock = PLAT_MUTEX_INIT};
static VkExec par_main_w;

typedef struct {
    ParCb cb;
    uint32_t fb_dirty;
    int recorded, hazard;
    ViewEnt *views;
    int nviews;
} ParOut;
static ParOut par_out[PAR_MAX_SLICES];

static void par_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
    case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
    case OP_PATCH_SIZE: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
        state_op(ex, op, a);
        break;
    default: vk_exec_op_draw(ex, op, a); break;
    }
}

static void par_rec_note(int worker, int i, uint64_t ns) {
    int end = PC.end[i], draws = 0;
    for (int k = PC.start[i]; k < end; k++) draws += PC.ops[k]->op == OP_DRAW || PC.ops[k]->op == OP_DRAW_IDX;
    plat_add64(&par_rec[worker][0], 1), plat_add64(&par_rec[worker][1], draws), plat_add64(&par_rec[worker][2], (int64_t)ns);
    if (ns > 1000000) {
        plat_add64(&par_slow[worker], 1);
        int64_t m = __atomic_load_n(&par_slow_ns, __ATOMIC_RELAXED);
        while ((int64_t)ns > m && !__atomic_compare_exchange_n(&par_slow_ns, &m, (int64_t)ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if ((int64_t)ns >= m) __atomic_store_n(&par_slow_draws, draws, __ATOMIC_RELAXED);
    }
}

static void par_record(int i, ParPool *pp, VkExec *w, struct DrawCtx *dc) {
    VkCommandBuffer cb = par_secondary(pp);
    par_begin_secondary(&PC.snap[i], cb, PC.soq[i], PC.soqf[i]);
    *w = PC.snap[i];
    w->draw = dc;
    w->cb = cb, w->recorded = 0, w->dirty = VK_EXEC_DIRTY_ALL;
    w->fb_dirty = 0x1ff;
    w->views = NULL, w->nviews = 0, w->cap_views = 0;
    struct DrawCtx *prev_dc = vk_exec_draw_slice_enter(dc);
    vk_exec_draw_rebase(w);
    vk_exec_draw_slice_view(dc, PC.supd[i]);
    vk_usnap_view(par_usd[PC.supd[i]] + 1);
    par_slice = 1, par_hazard = 0;
    vk_mem_count_quiet(1), vk_usnap_quiet(1);
    for (int k = PC.start[i], end = PC.end[i]; k < end; k++) par_op(w, PC.ops[k]->op, nvn_op_payload(PC.ops[k]));
    par_slice = 0;
    vk_mem_count_quiet(0), vk_usnap_quiet(0), vk_usnap_view(0);
    vk_exec_draw_slice_enter(prev_dc);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) rt_fatal("vk: secondary command buffer end failed");
    par_out[i] = (ParOut){{pp, cb}, w->fb_dirty, w->recorded, par_hazard, w->views, w->nviews};
}

static void par_publish(void) {
    plat_mutex_lock(&pw_lock);
    PC.end[PC.nslices - 1] = PC.nops;
    pw_pub = PC.nslices;
    if (par_nworkers) plat_cond_broadcast(&pw_go);
    plat_mutex_unlock(&pw_lock);
}

static void par_wake_workers(void) {
    if (!par_nworkers) return;
    plat_mutex_lock(&pw_lock);
    __atomic_store_n(&pw_active, 1, __ATOMIC_RELEASE);
    plat_cond_broadcast(&pw_go);
    plat_mutex_unlock(&pw_lock);
}

static void par_worker(void *arg) {
    ParWorker *pw = arg;
    struct DrawCtx *dc = vk_exec_draw_slice_ctx(pw->k);
    plat_mutex_lock(&pw_lock);
    for (;;) {
        while (pw_next >= pw_pub) {
            if (!__atomic_load_n(&pw_active, __ATOMIC_ACQUIRE)) {
                plat_cond_wait(&pw_go, &pw_lock);
                continue;
            }
            plat_mutex_unlock(&pw_lock);
            while (__atomic_load_n(&pw_active, __ATOMIC_ACQUIRE) &&
                   __atomic_load_n(&pw_next, __ATOMIC_RELAXED) >= __atomic_load_n(&pw_pub, __ATOMIC_RELAXED))
                plat_pause();
            plat_mutex_lock(&pw_lock);
        }
        int i = pw_next++;
        plat_mutex_unlock(&pw_lock);
        uint64_t t = kern_now_ns();
        par_record(i, &pw->pp, &pw->w, dc);
        par_rec_note(1, i, kern_now_ns() - t);
        plat_mutex_lock(&pw_lock);
        __atomic_store_n(&pdone[i], 1, __ATOMIC_RELEASE);
        plat_cond_broadcast(&pw_idle);
    }
}

static void par_start_workers(void) {
    int want = par_mode - 1;
    if (want > PAR_MAX_WORKERS) want = PAR_MAX_WORKERS;
    if (par_nworkers >= want) return;
    if (!pw_conds) plat_cond_init(&pw_go), plat_cond_init(&pw_idle), pw_conds = 1;
    while (par_nworkers < want) {
        ParWorker *pw = calloc(1, sizeof *pw);
        if (!pw) rt_fatal("vk: out of memory");
        pw->k = par_nworkers + 1;
        plat_mutex_init(&pw->pp.lock);
        vk_exec_draw_slices_begin(pw->k + 1, -1);
        vk_mem_threads();
        if (!(pw->t = plat_thread_new_ex(par_worker, pw, "replay slices", 0))) {
            rt_log("vk: a replay slice worker could not start: %d of %d run", par_nworkers, want);
            free(pw);
            par_mode = par_nworkers + 1;
            return;
        }
        par_workers[par_nworkers++] = pw;
    }
    rt_log("vk: %d replay slice workers started", par_nworkers);
}

static int par_threads(void) {
    if (par_mode < 0) {
        const char *e = cfg_get("MTF_REPLAY_THREADS");
        uint32_t cpus = plat_cpu_usable();
        par_mode = e && *e ? atoi(e)
                 : (vk_caps.amd || vk_ctx()->props.vendorID == 0x10de) ? 0
                 : cpus >= 12 ? 6 : cpus >= 8 ? 4 : 0;
        if (par_mode < 0) par_mode = 0;
        if (par_mode)
            rt_log("vk: passes of %d or more draws are recorded in slices of up to %d draws into secondary command buffers "
                   "(MTF_REPLAY_THREADS=%d%s: recorded by that many threads)", PAR_MIN_DRAWS, PAR_SLICE_DRAWS, par_mode,
                   e && *e ? "" : ", the default");
    }
    return par_mode;
}

static int par_size(void) {
    if (par_mode <= 1) return PAR_SLICE_DRAWS;
    int size = (PC.expect - PC.draws) / par_mode;
    return size < PAR_SLICE_MIN ? PAR_SLICE_MIN : size > PAR_SLICE_DRAWS ? PAR_SLICE_DRAWS : size;
}

static int par_start(VkExec *ex) {
    par_threads();
    if (!par_mode || ex != &X || (ex->in_pass && pass_tried)) return 0;
    pass_tried = 1;
    if (vk_dump_armed) return 0;
    resolve_rt(ex);
    uint64_t sig = par_sig(ex);
    ParHist *e = par_find(sig, 0);
    if (!e || e->draws < PAR_MIN_DRAWS || e->banned >= PAR_BAN_AFTER || ex->pass_noatt) return 0;
    if (!vk_exec_oq_sliceable()) { par_skipped_oq++; return 0; }
    if (ex->in_pass) {
        par_restart = 1;
        vk_exec_pass_end(ex);
        par_restart = 0;
        par_restarts++;
    }
    VkQueryControlFlags qf = 0;
    int qopen = 0, q = vk_exec_oq_begin_outside(ex, &qf, &qopen);
    if (q < 0) { par_skipped_oq++; return 0; }
    pass_secondary = 1;
    vk_exec_pass_begin(ex);
    pass_secondary = 0;
    pass_tried = 1;
    if (!ex->in_pass) { par_no[4]++; vk_exec_oq_pass_ended(ex); return 0; }
    PC.oq = qopen, PC.oq_flags = qf;
    if (!PC.ops) {
        PC.ops = malloc(sizeof *PC.ops * PAR_MAX_OPS);
        PC.snap = malloc(sizeof *PC.snap * PAR_MAX_SLICES);
        if (!PC.ops || !PC.snap) rt_fatal("vk: out of memory");
    }
    if (par_mode > 1) par_start_workers();
    plat_mutex_lock(&pw_lock);
    pw_pub = pw_next = 0;
    plat_mutex_unlock(&pw_lock);
    PC.nslices = PC.nops = PC.s0 = 0;
    PC.upd = (int)(par_pass_seq++ % VK_EXEC_UPD_SNAPS);
    par_usd[PC.upd] = vk_usnap_drops();
    vk_exec_draw_slices_begin(par_nworkers + 1, PC.upd);
    uint32_t *x = par_expect(par_piece_key(sig, 0), 0);
    PC.expect = (int)(x && *x ? *x : e->draws);
    PC.on = 1, PC.sig = sig, PC.draws = 0;
    PC.slice_size = PC.slice_draws = par_size();
    par_t0 = kern_now_ns();
    par_wake_workers();
    return 1;
}

static int par_take(VkExec *ex, const op_hdr *h, const void *a) {
    if (PC.nops == PAR_MAX_OPS || vk_dump_armed) return 0;
    switch (h->op) {
    case OP_SET_RT: return !memcmp(&ex->rt, a, sizeof ex->rt);
    case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
    case OP_PATCH_SIZE: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
        state_op(ex, h->op, a);
        break;
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
        vk_exec_op_draw(ex, h->op, a);
        break;
    case OP_DRAW: case OP_DRAW_IDX:
        if (PC.slice_draws >= PC.slice_size && PC.nslices < PAR_MAX_SLICES) {
            if (PC.nslices > PC.s0) par_publish();
            int i = PC.nslices++;
            PC.snap[i] = *ex;
            PC.start[i] = PC.nops;
            PC.soq[i] = (uint8_t)PC.oq, PC.soqf[i] = PC.oq_flags, PC.supd[i] = (uint8_t)PC.upd;
            __atomic_store_n(&pdone[i], 0, __ATOMIC_RELAXED);
            PC.slice_draws = 0;
            PC.slice_size = par_size();
        }
        PC.slice_draws++, PC.draws++;
        vk_exec_frame.draws_seen++;
        vk_dlaa_draw(ex);
        break;
    default: return 0;
    }
    PC.ops[PC.nops++] = h;
    return 1;
}

static void par_flush(VkExec *ex) {
    if (!PC.on) return;
    PC.on = 0;
    par_passes++, par_slices += (uint64_t)PC.nslices, par_sliced_draws += (uint64_t)PC.draws;
    uint64_t t1 = kern_now_ns();
    par_t_collect += t1 - par_t0;
    if (PC.nslices) par_publish();
    plat_mutex_lock(&pw_lock);
    while (pw_next < PC.nslices) {
        int i = pw_next++;
        plat_mutex_unlock(&pw_lock);
        uint64_t t = kern_now_ns();
        par_record(i, &par_main_pool, &par_main_w, vk_exec_draw_slice_ctx(0));
        par_rec_note(0, i, kern_now_ns() - t);
        plat_mutex_lock(&pw_lock);
        __atomic_store_n(&pdone[i], 1, __ATOMIC_RELEASE);
    }
    uint64_t t2 = kern_now_ns();
    for (int i = PC.s0; i < PC.nslices; i++)
        while (!__atomic_load_n(&pdone[i], __ATOMIC_ACQUIRE)) plat_cond_wait(&pw_idle, &pw_lock);
    uint64_t t3 = kern_now_ns();
    par_t_own += t2 - t1, par_t_wait += t3 - t2;
    if (t3 - t2 > par_wait_max) par_wait_max = t3 - t2;
    par_wait_long += t3 - t2 > 200000;
    __atomic_store_n(&pw_active, 0, __ATOMIC_RELEASE);
    plat_mutex_unlock(&pw_lock);

    vk_exec_draw_slices_end(par_nworkers + 1);
    int n = PC.nslices - PC.s0;
    ParDone *done = malloc(sizeof *done + sizeof(ParCb) * (size_t)(n ? n : 1));
    VkCommandBuffer *cbs = malloc(sizeof *cbs * (size_t)(n ? n : 1));
    if (!done || !cbs) rt_fatal("vk: out of memory");
    done->n = 0;
    int hazard = 0;
    for (int i = PC.s0; i < PC.nslices; i++) {
        ParOut *o = &par_out[i];
        done->cb[done->n++] = o->cb;
        cbs[i - PC.s0] = o->cb.cb;
        ex->fb_dirty |= o->fb_dirty;
        ex->recorded += o->recorded;
        if (o->hazard > hazard) hazard = o->hazard;
        for (int k = 0; k < o->nviews; k++) {
            if (ex->nviews == ex->cap_views) ex->views = par_grow(ex->views, &ex->cap_views, sizeof(ViewEnt));
            ((ViewEnt *)ex->views)[ex->nviews++] = o->views[k];
        }
        free(o->views);
    }
    uint64_t te = kern_now_ns();
    rec_drain();
#ifdef vkCmdExecuteCommands
#undef vkCmdExecuteCommands
#endif
    if (n > 0) vkCmdExecuteCommands(ex->cb, (uint32_t)n, cbs);
    free(cbs);
    ex->recorded++;
    vk_completion_add(ex->fence, par_done, done);
    par_t_exec += kern_now_ns() - te;
    vk_exec_draw_new_cb();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    pass_draws = (uint32_t)PC.draws;
    vk_exec_pass_end(ex);
    split_draws += PC.draws;
    if (hazard) {
        ParHist *e = par_find(PC.sig, 1);
        par_no[3]++;
        if (e->banned < 255 && ++e->banned == PAR_BAN_AFTER)
            rt_log("vk: a sliced pass needed a pass or barrier change inside a slice %d times: those targets are not sliced again",
                   PAR_BAN_AFTER);
    }
}

static void walk(VkExec *ex, const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (PC.on) {
            if (par_take(ex, h, a)) { h = nvn_op_next(h); continue; }
            par_flush(ex);
        }
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_SET_RT: set_rt(ex, a); break;
        case OP_CLEAR_COLOR: clear_color(ex, a); break;
        case OP_CLEAR_DS: clear_ds(ex, a); break;
        case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET:
        case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
        case OP_PATCH_SIZE: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
            state_op(ex, h->op, a);
            break;
        case OP_BARRIER: vk_exec_barrier(ex); break;
        case OP_DRAW: case OP_DRAW_IDX:
            if (ex == &X && (!ex->in_pass || !pass_tried) && par_start(ex)) {
                par_take(ex, h, a);
                h = nvn_op_next(h);
                continue;
            }
            pass_draws++;
            // fall through
        case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
        case OP_DRAW_IND_MULTI:
            if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND || h->op == OP_DRAW_IND_MULTI)
                vk_exec_frame.draws_seen++, split_draws++;
            if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, split_draws++;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
            vk_exec_op_draw(ex, h->op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
            if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
            // fall through
        case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        case OP_CLEAR_BUF:
            vk_exec_op_xfer(ex, h->op, a);
            break;
        default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
        }
        if (split_draws >= split_n) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    const tex_t *keep[9];   // the render targets carry over to this submission (set_rt compares them by pointer)
    memcpy(keep, ex->rt.color, sizeof ex->rt.color);
    keep[8] = ex->rt.depth;
    nvn_pins_sweep(nvn_submit_count(), keep, 9);
    ex->cb = cb;
    ex->fence = fence;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->mark = NULL;
    ex->mark_passes = ex->mark_dispatches = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++;
    split_draws = part_start = 0;
    part_t0 = kern_now_ns();
    x_here = 1;
    if (n) vk_mem_mirror_begin(), vk_usnap_begin(handles, n, fence);
    for (int i = 0; i < n; i++) {
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
        walk(ex, nvn_op_first(handles[i]), 0);
    }
    par_flush(ex);
    vk_exec_pass_end(ex);
    if (n) vk_dlaa_submission_end(ex);
    x_here = 0;
    if (ex->recorded) vk_exec_barrier(ex);   // later submissions see this one's writes
    if (n) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    if (ex->nviews) {
        ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
        if (!l) rt_fatal("vk: out of memory");
        l->n = ex->nviews;
        for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
        vk_completion_add(fence, free_views, l);
        ex->nviews = 0;
    }
    ex->cb = VK_NULL_HANDLE;
    if (n) n_subs++;
    if (n && (rt_trace & RT_TRACE_PERF)) vk_exec_ub_perf();
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f", (double)(n_subs + n_parts) / (double)n_subs);
        if (par_mode > 0) {
            rt_log("perf: parallel passes %llu (restarts %llu), slices %llu, sliced draws %llu, skipped oq %llu; collect %.2f ms, "
                   "own %.2f ms, wait %.2f ms (max %.2f ms, >200us %llu), exec %.2f ms",
                   (unsigned long long)par_passes, (unsigned long long)par_restarts, (unsigned long long)par_slices,
                   (unsigned long long)par_sliced_draws, (unsigned long long)par_skipped_oq, par_t_collect / 1e6,
                   par_t_own / 1e6, par_t_wait / 1e6, par_wait_max / 1e6, (unsigned long long)par_wait_long,
                   par_t_exec / 1e6);
            par_passes = par_restarts = par_slices = par_sliced_draws = par_skipped_oq = 0;
            par_t_collect = par_t_own = par_t_wait = par_t_exec = par_wait_max = par_wait_long = 0;
        }
        n_subs = n_parts = 0;
    }
    return ex->recorded - part_start;   // what the last part holds
}
