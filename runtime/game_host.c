// Binds lifted game metadata from mtf-game.dll (split Windows build) or statically linked modules.c.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_abi.h"

#ifndef MTF_PORT_VERSION
#define MTF_PORT_VERSION "0.1.0"
#endif

const char rt_port_version[] = MTF_PORT_VERSION;

const RtLayoutEntry *rt_layout = NULL;
int rt_layout_count = 0;
const RtFileHash *rt_exefs_sha256 = NULL;
int rt_exefs_sha256_count = 0;
const char *rt_default_exefs = "";
const char *rt_default_romfs = "";
const char *rt_default_profile = "";
int rt_game_version = 0;

static int (*g_linked_modules)(RtModule *out, int max) = NULL;

int rt_linked_modules(RtModule *out, int max) {
    return g_linked_modules ? g_linked_modules(out, max) : 0;
}

static void game_host_bind(const MtfGameDesc *d) {
    if (!d) rt_fatal("game_host: mtf_game_desc() returned NULL");
    if (d->abi_version != MTF_GAME_ABI_VERSION)
        rt_fatal("game_host: mtf-game.dll ABI %d does not match runtime ABI %d (full update required)",
                 d->abi_version, MTF_GAME_ABI_VERSION);
    rt_layout = d->layout;
    rt_layout_count = d->layout_count;
    g_linked_modules = d->linked_modules;
    rt_exefs_sha256 = d->exefs_sha256;
    rt_exefs_sha256_count = d->exefs_sha256_count;
    rt_default_exefs = d->default_exefs ? d->default_exefs : "";
    rt_default_romfs = d->default_romfs ? d->default_romfs : "";
    rt_default_profile = d->default_profile ? d->default_profile : "";
    rt_game_version = d->game_version;
}

#ifdef MTF_RUNTIME_SPLIT
void game_host_load(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) rt_fatal("game_host: GetModuleFileNameA failed");
    char *s = strrchr(path, '\\');
    char *f = strrchr(path, '/');
    if (f && (!s || f > s)) s = f;
    if (!s) rt_fatal("game_host: bad exe path %s", path);
    snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "mtf-game.dll");

    HMODULE dll = LoadLibraryA(path);
    if (!dll) rt_fatal("game_host: could not load %s (Windows error %lu)", path, (unsigned long)GetLastError());

    typedef const MtfGameDesc *(*DescFn)(void);
    DescFn fn = (DescFn)GetProcAddress(dll, "mtf_game_desc");
    if (!fn) rt_fatal("game_host: mtf_game_desc export missing in %s", path);
    game_host_bind(fn());
}
#else
extern const MtfGameDesc *mtf_game_desc(void);
void game_host_load(void) {
    game_host_bind(mtf_game_desc());
}
#endif
