// Settings: MTF_* environment variables, with mtf.ini next to mtf-recomp.exe behind them.
//
// The interface (cfg.h) is the subset of settings calls the runtime uses.
//
// mtf.ini: "key = value" lines; '#' or ';' starts a comment line; [sections] only group. A key is the variable name
// without "MTF_" (an MTF_ prefix is accepted), case-insensitive. An empty value counts as unset. The environment wins.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define PREFIX "MTF_"
#define PREFIX_LEN (sizeof PREFIX - 1)
#define CFG_MAX_KEYS 256

typedef struct { char *key, *value; } CfgEntry;   // key upper-case, without the prefix
static CfgEntry entries[CFG_MAX_KEYS];
static int n_entries, loaded;

// Defaults the runtime relies on when a key is unset. A key that is not here has no default (cfg_get returns NULL).
static const struct { const char *key, *def; } defaults[] = {
    {"ASYNC_PREEMPT", "1"},   // kernel.c: interrupt a core holder that runs game code for too long
    {"SPIN", "1"},            // kernel.c rt_spin_gate: give the core away in spin-wait loops
    {"FREE_CORES", "0"},      // kernel.c: 1 lifts the virtual core gate (experiments only)
    {"TIMER_1MS", "1"},       // main.c: 1 ms timer resolution for timed guest waits
    {"OPERATION_MODE", "docked"},   // ov_system.c: nn::oe::GetOperationMode (docked or handheld)
    {"ASSUME_FOCUS", "1"},    // ov_system.c: the game keeps running while the window is not focused
    {"MOTION", "1"},          // input.c: the gamepad's gyro and accelerometer feed the six-axis sensor
    {"WINDOW_SIZE", "1920x1080"},   // gfx_present.c: the window's client size in windowed mode
    {"VOLUME", "100"},        // audio_mixer.c: master volume, 0-100
    {"RENDER_SCALE", "100"},        // ov_resolution.c: the scene's size in percent of the output (50-100), or game (80 docked)
    {"OUTPUT_RES", "auto"},         // ov_resolution.c: the game's output, 720, 1080, 1440 or 2160 lines; auto: the display's
    {"GRAPHICS_PRESET", "custom"},  // menu.cpp: low | balanced | high | ultra | custom
    {"DLSS", "dlaa"},               // vk_dlaa.c: with DLAA on, dlaa (scene size) or quality, balanced, performance,
                                    // ultra_performance (the scene at NVIDIA's share of the output)
    {"AUDIO_CHANNELS", "auto"},     // audio_mixer.c, ov_audioout.c: auto (the device's layout), stereo or 5.1
    // Display. vk_color.c reads these four on every present: unset, contrast, saturation and
    // gamma read as 0, and gamma 0 turned every pixel white
    {"COLOR_BRIGHTNESS", "0"},      // -30..30, % added
    {"COLOR_CONTRAST", "100"},      // %, 100 = unchanged
    {"COLOR_SATURATION", "100"},    // %, 100 = unchanged
    {"COLOR_GAMMA", "1"},           // exponent, 1 = unchanged
    {"FULLSCREEN", "0"},            // gfx_present.c (F11 toggles)
    {"WINDOW_MODE", "windowed"},    // gfx_window_win.c: windowed, borderless, fullscreen
    {"UPSCALER", "fsr"},            // vk_fsr.c: FSR 1 (or bilinear) when the window is larger than the game's output
    {"SHARPNESS", "50"},
    {"ASPECT", "fit"},              // fit (bars) or stretch
    {"ULTRAWIDE", "off"},           // ov_resolution.c: off (16:9), 21:9, or 32:9
    {"HUD_POSITION", "edges"},      // edges or centered (16:9 safe zone in ultrawide)
    {"FOV", "100"},                 // ov_resolution.c: camera FOV % (70..140, 100 = game default)
    {"MOVIE_ASPECT", "keep"},
    {"ANISO", "8"},                 // vk_texture.c: the game's anisotropy, or 2..16
    {"SHADOW_RES", "game"},
    {"SHADOW_SCALE", "2"},
    {"VATTR_LOG", "0"},             // (diagnostic) vk_pipeline.c logs each vertex attribute format once
    {"PROBE_VS", ""},               // (diagnostic) vk_exec_draw.c: a vertex shader key (or "skinned") whose draws are logged
    {"CMD_MEM_CB", "1"},            // memory callbacks when a recording runs out of command / control memory (nvn_api_cmd.c)
    {"GEOMETRY", "1"},              // draws use their programs' geometry stage (vk_pipeline.c, vk_exec_draw.c)
    {"TESS", "1"},                  // draws use their programs' tessellation stages: the grass and the sea of clouds
    {"SWAPCHAIN_SRGB", "1"},        // the window textures are sRGB: an sRGB swapchain keeps the blit from darkening them
    {"HDR", "0"},                   // vk_hdr.c: HDR10 output (read at start); the game itself is SDR
    {"HDR_PEAK_NITS", "1000"},      // the display's peak
    {"HDR_PAPER_WHITE", "115"},     // the picture's white
    {"HDR_UI_NITS", "160"},         // the HUD's and the menus' white
    {"FPS_LIMIT", "60"},
    {"FRAMEGEN", "off"},
    {"BACKGROUND_FPS", "15"},       // gfx_present.c: with no focus and no controller in use; 0 for no limit
    {"VK_PRESENT", "mailbox"},       // gfx_present.c: fifo (VSync), mailbox or immediate; the menu changes it live
    {"FRAME_QUEUE", "0"},
    {"PAD_LAYOUT", "auto"},
    {"STICK_DEADZONE", "8"},
    {"MULTIPLAYER_INPUT", "0"},     // input.c, ov_hid.c: 1 enables per-player input routing (P1..P4)
    {"P1_INPUT", "auto"},           // input.c: auto | none | keyboard | pad:0..pad:3
    {"P2_INPUT", "none"},
    {"P3_INPUT", "none"},
    {"P4_INPUT", "none"},
    // vk_exec.c: a masked stencil clear (the game clears bits 0xf0 every frame) clears every bit. A stopgap until the
    // masked clear is replayed as a draw: skipped, the sky stayed dark (open: the low bits are lost)
    {"STENCIL_MASK_FULL", "1"},
    // ov_system.c: the game's language (nn::oe::GetDesiredLanguage). English unless set: following Windows gave "pt"
    // (European Portuguese) on a pt-BR system, which the game does not ship, and it fell back to Japanese. auto (or
    // MTF_LOCALE set) follows the locale.
    {"LANG", "en-US"},
    // The Vulkan back end's switches and their defaults. The code reads most of
    // them with cfg_bool or cfg_int, so unset meant off: until 2026-11-07 the port ran with c0 zeroed (FIX_C0: a
    // storage buffer 8 bytes past a 16-byte boundary was read 8 bytes early, which put the NPC blend-shape faces on the
    // floor), no zero fill of new pools, no write-back merge and the draw-path memos off. VK_PRESENT defaults to fifo
    // (above).
    {"MEMORY", "auto"},             // vk_memory.c: auto, import, mirror or verify
    {"ASTC", "auto"},               // vk_astc.c: auto, gpu, native or cpu
    {"FIX_C0", "1"},                // vk_exec_draw.c: the driver constant bank c0 (storage addresses, dispatch size,
                                    // base instance, point size) instead of zeros
    {"FIX_ZERO", "1"},              // vk_memory.c, vk_texture.c: new pool memory and images start zeroed, as on the Switch
    {"TEX_ALIAS", "1"},             // vk_texture.c: a new image over a live image's storage takes its content
    {"TEX_VRAM_PRESSURE", "90"},    // vk_texture.c: released textures are dropped above this % of the VRAM budget
    {"TEX_RELEASED_MIB", "256"},    // vk_texture.c: and while they hold more own memory than this (0: no cap)
    {"WB_EXACT", "1"},              // vk_texture.c: a render target that fills no 4 KiB page keeps a copy of its storage,
                                    // so a neighbour's CPU write to a shared page does not cancel its write-back
    {"WB_MERGE", "1"},              // vk_memory.c: GPU writes go back to guest memory word by word against a snapshot,
                                    // so the words the GPU left alone keep the CPU's value
    {"WB_FENCE_LIMIT", "65536"},    // vk_texture.c: render targets up to this many bytes go back when the CPU waits on
                                    // a fence
    {"VB_USED", "1"},               // vk_exec_draw.c: only the streams the draw's vertex input reads are bound
    {"DYN_DELTA", "1"},             // vk_exec_draw.c: only the dynamic states that changed since the last draw state
    {"PUSH_SKIP", "1"},             // vk_exec_draw.c: a draw with the same descriptor set as the last one pushes nothing
    {"PIPE_MEMO", "1"},             // vk_exec_draw.c: the previous draw's pipeline is reused while its state is equal
    {"DESC_TEMPLATE", "1"},         // vk_pipeline.c: descriptor update templates
    {"LIB_PREBUILD", "1"},          // vk_pipeline.c: compute pipelines and stage libraries of known shaders built at boot
    {"PIPE_PREBUILD", "1"},         // vk_pipeline.c: graphics pipelines of earlier runs (pipeline-keys.bin) built at boot
    {"PIPE_SKIP", "fullscreen"},    // vk_exec_draw.c: which draws are skipped while their pipeline builds
    {"PIPE_WAIT", "default"},       // vk_pipeline.c: pipelines build on worker threads (draw: on the drawing thread)
    {"SPLIT_DRAWS", "64"},          // vk_exec.c: the replay's command buffer is submitted in parts of this many draws...
    {"SPLIT_US", "1000"},           // ...and at least this many microseconds of recording, so the GPU starts early
    {"VK_SUBGROUP_PIN", "1"},       // vk_caps.c: pin the subgroup size the shaders were translated for
    {"CHEAT_UNLOCK_CHARS", "0"},    // cheats.c: unlock all characters in memory
    {"CHEAT_UNLOCK_COSTUMES", "0"}, // cheats.c: unlock all alternate character costumes/colors
    {"CHEAT_UNLOCK_RACKETS", "0"},  // cheats.c: unlock all rackets in memory
    {"CHEAT_UNLOCK_COURTS", "0"},   // cheats.c: unlock all courts in memory
    {"CHEAT_UNLOCK_MODES", "0"},    // cheats.c: unlock all tournament cups and modes
};

// The settings the menu shows (ui/menu.cpp), in its order: what each accepts and whether it applies at once. A NULL
// default is the one in the table above; the others are the only place their default lives.
#define LIVE 1
#define RESTART 0
static CfgMeta metas[] = {
    // Screen
    {"WINDOW_MODE", NULL, "windowed|borderless|fullscreen", LIVE}, {"FULLSCREEN", NULL, "0|1", LIVE}, {"WINDOW_SIZE", NULL, NULL, LIVE},
    {"VK_PRESENT", NULL, "mailbox|fifo|immediate", LIVE}, {"GPU_INDEX", "", NULL, RESTART},
    {"FPS_LIMIT", NULL, "30|45|60|90|120|display|unlimited", LIVE},
    {"FRAMEGEN", NULL, "off|120|monitor|unlimited", LIVE},
    {"PERF_OVERLAY", "off", "off|fps|full", LIVE},
    {"BACKGROUND_FPS", NULL, "0|10|15|20|30", LIVE},
    {"OUTPUT_RES", NULL, "auto|720|1080|1440|2160|2880|4320", RESTART},
    {"ULTRAWIDE", NULL, "off|21:9|32:9", RESTART},
    {"HUD_POSITION", NULL, "edges|centered", LIVE},
    {"FOV", NULL, "70..140", LIVE},
    {"RENDER_SCALE", NULL, "game|50|67|75|100|125|150|200", RESTART},
    {"GRAPHICS_PRESET", "custom", "low|balanced|high|ultra|custom", RESTART},
    {"UPSCALER", NULL, "fsr|bilinear", LIVE}, {"SHARPNESS", NULL, "0..100", LIVE},
    {"ASPECT", NULL, "fit|stretch", LIVE}, {"ANISO", NULL, "game|2|4|8|16", RESTART},
    {"SHADOW_SCALE", "2", "1|2|3|4", RESTART},
    {"TESS", NULL, "0|1", RESTART},
    {"DLAA", "0", "0|1", RESTART}, {"DLSS", NULL, "dlaa|quality|balanced|performance|ultra_performance", RESTART},
    {"DLSS_PRESET", "default", "default|k|j", LIVE}, {"FXAA", "1", "0|1", LIVE},
    {"SMAA", "medium", "off|low|medium|high", LIVE},
    {"COLOR_PRESET", "default", "default|vivid|clarity|custom", LIVE}, {"COLOR_BRIGHTNESS", NULL, "-30..30", LIVE},
    {"COLOR_CONTRAST", NULL, "70..150", LIVE}, {"COLOR_SATURATION", NULL, "50..200", LIVE},
    {"COLOR_GAMMA", NULL, "0.8..1.2", LIVE},
    {"HDR", NULL, "0|1", RESTART}, {"HDR_PEAK_NITS", NULL, "400..10000", LIVE}, {"HDR_PAPER_WHITE", NULL, "80..500", LIVE},
    {"HDR_UI_NITS", NULL, "80..500", LIVE},
    // Game
    {"FRAME_QUEUE", NULL, "0|1", LIVE},
    {"LANG", NULL, "en-US|en-GB|ja|fr|fr-CA|de|it|es|es-419|pt-BR|nl|ko|zh-Hans|zh-Hant|auto", RESTART},
    {"OPERATION_MODE", NULL, "docked|handheld", RESTART}, {"ASSUME_FOCUS", NULL, "0|1", LIVE},
    // Sound, Controls, Menu
    {"VOLUME", NULL, "0..100", LIVE}, {"AUDIO_CHANNELS", NULL, "auto|stereo|5.1", RESTART},
    {"PAD_LAYOUT", NULL, "auto|positional", LIVE}, {"BUTTON_ICONS", "auto", "auto|nintendo|xbox|playstation|keyboard", LIVE},
    {"MULTIPLAYER_INPUT", NULL, "0|1", LIVE},
    {"STREAM_QUALITY", "balanced", "performance|balanced|quality", LIVE},
    {"STREAM_CODEC", "auto", "auto|av1|h264|vp8", LIVE},
    {"P1_INPUT", NULL, NULL, LIVE}, {"P2_INPUT", NULL, NULL, LIVE},
    {"P3_INPUT", NULL, NULL, LIVE}, {"P4_INPUT", NULL, NULL, LIVE},
    {"BIND_A", "", NULL, LIVE}, {"BIND_B", "", NULL, LIVE}, {"BIND_X", "", NULL, LIVE}, {"BIND_Y", "", NULL, LIVE},
    {"BIND_LCLICK", "", NULL, LIVE}, {"BIND_RCLICK", "", NULL, LIVE}, {"BIND_L", "", NULL, LIVE}, {"BIND_R", "", NULL, LIVE},
    {"BIND_ZL", "", NULL, LIVE}, {"BIND_ZR", "", NULL, LIVE}, {"BIND_PLUS", "", NULL, LIVE}, {"BIND_MINUS", "", NULL, LIVE},
    {"BIND_LEFT", "", NULL, LIVE}, {"BIND_UP", "", NULL, LIVE}, {"BIND_RIGHT", "", NULL, LIVE}, {"BIND_DOWN", "", NULL, LIVE},
    {"BIND_LSTICK_LEFT", "", NULL, LIVE}, {"BIND_LSTICK_UP", "", NULL, LIVE},
    {"BIND_LSTICK_RIGHT", "", NULL, LIVE}, {"BIND_LSTICK_DOWN", "", NULL, LIVE},
    {"BIND_RSTICK_LEFT", "", NULL, LIVE}, {"BIND_RSTICK_UP", "", NULL, LIVE},
    {"BIND_RSTICK_RIGHT", "", NULL, LIVE}, {"BIND_RSTICK_DOWN", "", NULL, LIVE},
    {"BIND_SHAKE", "", NULL, LIVE},
    {"MOTION", NULL, "0|1", LIVE},
    {"RUMBLE_STRENGTH", "100", "0..300", LIVE}, {"RUMBLE_CURVE", "1", "0.2..1.5", LIVE},
    {"STICK_DEADZONE", NULL, "0..50", LIVE},
    {"MENU_LANG", "", "en|pt-BR", LIVE},
    // Advanced: scheduling
    {"ASYNC_PREEMPT", NULL, "0|1|2", RESTART}, {"REPLAY_ASYNC", "0", "0|1", RESTART}, {"TIMER_1MS", NULL, "0|1", RESTART},
    // shaders and pipelines
    {"PIPE_SKIP", NULL, "strict|any|fullscreen", RESTART}, {"PIPE_PREBUILD", NULL, "0|1", RESTART},
    {"GPL", "0", "0|1", RESTART},
    // textures and memory
    {"ASTC", NULL, "auto|gpu|native|cpu", RESTART}, {"MEMORY", NULL, "auto|import|mirror|verify", RESTART},
    {"SPARSE_MIRROR_COPY", NULL, "0|1", RESTART}, {"COMMIT_HEADROOM_MIB", "512", NULL, RESTART},
    {"TEX_ALIAS", NULL, "0|1", RESTART}, {"TEX_VRAM_PRESSURE", NULL, "0..100", RESTART}, {"ASTC_BC", "", "0|1", RESTART},
    // render fixes
    {"FIX_C0", NULL, "0|1", RESTART}, {"FIX_ZERO", NULL, "0|1", RESTART}, {"BIND_SKIP", "1", "0|1", RESTART},
    {"SHADER_MEMO", "1", "0|1", RESTART}, {"PIPE_MEMO", NULL, "0|1", RESTART}, {"TEX_MEMO", "1", "0|1", RESTART},
    {"DESC_TEMPLATE", NULL, "0|1", RESTART}, {"PUSH_SKIP", NULL, "0|1", RESTART}, {"SPLIT_DRAWS", NULL, "0..1000", RESTART},
    // Vulkan
    {"VK_VALIDATION", "0", "0|1", RESTART}, {"VK_DYN_VERTEX", "", "0|1", RESTART}, {"VK_DYN_A2C", "", "0|1", RESTART},
    {"VK_SUBGROUP_PIN", NULL, "0|1", RESTART}, {"VRAM_HEADROOM", "1", "0|1", RESTART}, {"HEAP_LAZY", "1", "0|1", RESTART},
    {"REPLAY_THREADS", "", "0..16", RESTART},
    {"NAN_GUARD", "0", "0|zero|mark", RESTART},
    // cheats
    {"CHEAT_UNLOCK_CHARS", NULL, "0|1", LIVE}, {"CHEAT_UNLOCK_COSTUMES", NULL, "0|1", LIVE},
    {"CHEAT_UNLOCK_RACKETS", NULL, "0|1", LIVE}, {"CHEAT_UNLOCK_COURTS", NULL, "0|1", LIVE},
    {"CHEAT_UNLOCK_MODES", NULL, "0|1", LIVE},
    // diagnostics
    {"TRACE", "", NULL, RESTART},
};

static const char *table_default(const char *bare) {
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++)
        if (!strcmp(defaults[i].key, bare)) return defaults[i].def;
    return NULL;
}

const CfgMeta *cfg_meta(const char *key) {
    const char *bare = strncasecmp(key, PREFIX, PREFIX_LEN) ? key : key + PREFIX_LEN;
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++)
        if (!strcasecmp(metas[i].key, bare)) {
            if (!metas[i].def) {   // the default lives in the table above (the same string every time: no race)
                const char *d = table_default(metas[i].key);
                metas[i].def = d ? d : "";
            }
            return &metas[i];
        }
    return NULL;
}

const CfgMeta *cfg_meta_at(int i) {
    if (i < 0 || (size_t)i >= sizeof metas / sizeof *metas) return NULL;
    return cfg_meta(metas[i].key);
}

static char *dup_upper(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) rt_fatal("cfg: allocation");
    for (size_t i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)s[i]);
    out[n] = 0;
    return out;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static void parse(char *text) {
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (!strncasecmp(key, PREFIX, PREFIX_LEN)) key += PREFIX_LEN;
        size_t vn = strlen(value);
        if (vn >= 2 && value[0] == '"' && value[vn - 1] == '"') value[vn - 1] = 0, value++;
        if (!*key || n_entries == CFG_MAX_KEYS) continue;
        entries[n_entries].key = dup_upper(key, strlen(key));
        entries[n_entries].value = *value ? strdup(value) : NULL;
        n_entries++;
    }
}

static const char *file_value(const char *bare);

void cfg_init(void) {
    if (loaded) return;
    loaded = 1;
    char path[PLAT_PATH_MAX * 3];
    int n = plat_exe_path(path, sizeof path);
    char *cut = n ? plat_last_sep(path) : NULL;
    if (!cut) return;
    strcpy(cut + 1, "mtf.ini");
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) return;
    char *copy = malloc(size + 1);
    if (!copy) rt_fatal("cfg: allocation");
    memcpy(copy, text, size);
    copy[size] = 0;
    free(text);
    parse(copy);
    free(copy);
    rt_log("cfg: %s, %d keys", path, n_entries);
    // An mtf.ini with the AA setting (off, fxaa or dlaa) gets the menu's two switches, DLAA and FXAA, in its place, and
    // one with RUMBLE = 0 a rumble strength of 0
    const char *aa = file_value("AA");
    if (aa) {
        if (!file_value("DLAA")) cfg_set("MTF_DLAA", !strcmp(aa, "dlaa") ? "1" : "0");
        if (!file_value("FXAA") && !strcmp(aa, "off")) cfg_set("MTF_FXAA", "0");
        cfg_set("MTF_AA", NULL);
    }
    const char *rumble = file_value("RUMBLE");
    if (rumble) {
        if (!strcmp(rumble, "0") && !file_value("RUMBLE_STRENGTH")) cfg_set("MTF_RUMBLE_STRENGTH", "0");
        cfg_set("MTF_RUMBLE", NULL);
    }
}

static const char *file_value(const char *bare) {
    for (int i = 0; i < n_entries; i++)
        if (!strcasecmp(entries[i].key, bare)) return entries[i].value;
    return NULL;
}

const char *cfg_get(const char *name) {
    const char *env = getenv(name);
    if (env && *env) return env;
    if (strncmp(name, PREFIX, PREFIX_LEN)) return NULL;
    return file_value(name + PREFIX_LEN);
}

int cfg_from_env(const char *name) {
    const char *env = getenv(name);
    return env && *env;
}

const char *cfg_value(const char *name) {
    const char *v = cfg_get(name);
    if (v) return v;
    const char *bare = strncmp(name, PREFIX, PREFIX_LEN) ? name : name + PREFIX_LEN;
    const char *d = table_default(bare);
    if (d) return d;
    const CfgMeta *m = cfg_meta(bare);
    return m ? m->def : "";
}

int cfg_int(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    long n = strtol(v, &end, 0);
    return *v && !*end ? (int)n : 0;
}

double cfg_double(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    double d = strtod(v, &end);
    return *v && !*end ? d : 0.0;
}

int cfg_bool(const char *name) {
    const char *v = cfg_value(name);
    return *v && strcmp(v, "0") != 0;
}

// Bumped by every live change (the in-game menu, F11): readers that keep a setting (vk_color.c, audio_mixer.c,
// gfx_present.c window_settings_live) read it again when it moves.
static volatile unsigned version;
unsigned cfg_version(void) { return __atomic_load_n(&version, __ATOMIC_ACQUIRE); }

// A setting changed while the game runs (the menu, F11's fullscreen): kept in memory for cfg_get until cfg_save writes
// mtf.ini. The environment still wins over it (cfg_from_env: the menu says so). Readers on other threads see the old
// or the new string, both valid: an old value is never freed.
void cfg_set(const char *name, const char *value) {
    const char *bare = strncmp(name, PREFIX, PREFIX_LEN) ? name : name + PREFIX_LEN;
    int i = 0;
    while (i < n_entries && strcasecmp(entries[i].key, bare)) i++;
    if (i < n_entries) {
        entries[i].value = value ? strdup(value) : NULL;
    } else if (n_entries < CFG_MAX_KEYS) {
        entries[n_entries].key = dup_upper(bare, strlen(bare));
        entries[n_entries].value = value ? strdup(value) : NULL;
        __atomic_store_n(&n_entries, n_entries + 1, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&version, 1, __ATOMIC_ACQ_REL);
}

int cfg_reset(const char *const *keys, int n) {
    int had = 0;
    for (int i = 0; i < n; i++) {
        const char *bare = strncasecmp(keys[i], PREFIX, PREFIX_LEN) ? keys[i] : keys[i] + PREFIX_LEN;
        if (file_value(bare)) had++, cfg_set(bare, NULL);
    }
    return had;
}

static volatile int save_ok = 1;
int cfg_last_save_ok(void) { return __atomic_load_n(&save_ok, __ATOMIC_ACQUIRE); }

// Writes every key set in mtf.ini or by cfg_set to mtf.ini beside the executable, one "KEY = value" line each, through
// a temporary file renamed over it. The file's comments do not survive. 1 on success.
static int save_to_ini(void);
int cfg_save(void) {
    int ok = save_to_ini();
    __atomic_store_n(&save_ok, ok, __ATOMIC_RELEASE);
    return ok;
}

static int save_to_ini(void) {
    char path[PLAT_PATH_MAX * 3], tmp[PLAT_PATH_MAX * 3 + 8];
    int n = plat_exe_path(path, sizeof path);
    char *cut = n ? plat_last_sep(path) : NULL;
    if (!cut) return 0;
    strcpy(cut + 1, "mtf.ini");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# MTF Recomp settings, written by the in-game menu (Minus+Plus, F1 or Esc).\n"
               "# KEY = value; the MTF_KEY environment variable wins over a line here.\n");
    int ok = 1;
    for (int i = 0; i < n_entries; i++)
        if (entries[i].value && fprintf(f, "%s = %s\n", entries[i].key, entries[i].value) < 0) ok = 0;
    if (fclose(f) || !ok) return remove(tmp), 0;
    if (!plat_file_replace(tmp, path)) return remove(tmp), 0;   // one step: a crash leaves the old file or the new
    rt_log("cfg: settings saved to %s", path);
    return 1;
}
