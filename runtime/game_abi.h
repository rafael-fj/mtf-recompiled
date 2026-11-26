// ABI contract between mtf-recomp.exe (runtime) and mtf-game.dll (lifted game code + modules.c).
#pragma once

#include "dispatch.h"
#include "host.h"

#define MTF_GAME_ABI_VERSION 1

typedef struct MtfGameDesc {
    int abi_version;
    const RtLayoutEntry *layout;
    int layout_count;
    int (*linked_modules)(RtModule *out, int max);
    const RtFileHash *exefs_sha256;
    int exefs_sha256_count;
    const char *default_exefs;
    const char *default_romfs;
    const char *default_profile;
    int game_version;
} MtfGameDesc;

#ifdef __cplusplus
extern "C" {
#endif

// Called once at startup by main.c before reading rt_layout or rt_default_*.
void game_host_load(void);

#ifdef __cplusplus
}
#endif
