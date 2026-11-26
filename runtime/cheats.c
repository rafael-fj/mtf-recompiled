#include "cheats.h"
#include "achievements.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "../profiles/0400D4201C6EA000/cheats_profile.h"

#include <string.h>

enum {
    CHEAT_CHARS = 0,
    CHEAT_COSTUMES,
    CHEAT_RACKETS,
    CHEAT_COURTS,
    CHEAT_MODES,
    CHEAT_COUNT
};

static const char *const kCheatKeys[CHEAT_COUNT] = {
    "MTF_CHEAT_UNLOCK_CHARS",
    "MTF_CHEAT_UNLOCK_COSTUMES",
    "MTF_CHEAT_UNLOCK_RACKETS",
    "MTF_CHEAT_UNLOCK_COURTS",
    "MTF_CHEAT_UNLOCK_MODES",
};

static uint8_t g_orig[MTF_UNLOCK_BITFIELD_SIZE];
static int g_snap_taken = 0;
static int g_prev_state[CHEAT_COUNT] = {0};

static int bitfield_writable(void) {
    uint64_t end = 0;
    int st = plat_mem_state(MTF_UNLOCK_BITFIELD_VA, MTF_UNLOCK_BITFIELD_VA + MTF_UNLOCK_BITFIELD_SIZE, &end);
    if (st != PLAT_PROT_RW) return 0;
    if (end < MTF_UNLOCK_BITFIELD_VA + MTF_UNLOCK_BITFIELD_SIZE) return 0;
    return plat_mem_readable(MTF_UNLOCK_BITFIELD_VA, MTF_UNLOCK_BITFIELD_SIZE);
}

static void apply_ranges(uint8_t *mem, const CheatBitRange *ranges, size_t n, int enable) {
    for (size_t i = 0; i < n; i++) {
        for (uint16_t b = ranges[i].bit_start; b <= ranges[i].bit_end; b++) {
            uint16_t byte_idx = b >> 3;
            uint8_t mask = (uint8_t)(0x80u >> (b & 7u));
            if (byte_idx >= MTF_UNLOCK_BITFIELD_SIZE) continue;
            if (enable) {
                mem[byte_idx] |= mask;
            } else {
                mem[byte_idx] = (uint8_t)((mem[byte_idx] & ~mask) | (g_orig[byte_idx] & mask));
            }
        }
    }
}

static void apply_category(uint8_t *mem, int cat, int enable) {
    switch (cat) {
    case CHEAT_CHARS:
        apply_ranges(mem, kCheatCharRanges, sizeof(kCheatCharRanges) / sizeof(kCheatCharRanges[0]), enable);
        break;
    case CHEAT_COSTUMES:
        apply_ranges(mem, kCheatCostumeRanges, sizeof(kCheatCostumeRanges) / sizeof(kCheatCostumeRanges[0]), enable);
        break;
    case CHEAT_RACKETS:
        apply_ranges(mem, kCheatRacketRanges, sizeof(kCheatRacketRanges) / sizeof(kCheatRacketRanges[0]), enable);
        break;
    case CHEAT_COURTS:
        apply_ranges(mem, kCheatCourtRanges, sizeof(kCheatCourtRanges) / sizeof(kCheatCourtRanges[0]), enable);
        break;
    case CHEAT_MODES:
        apply_ranges(mem, kCheatModeRanges, sizeof(kCheatModeRanges) / sizeof(kCheatModeRanges[0]), enable);
        break;
    }
}

int cheats_available(void) {
    return bitfield_writable();
}

void cheats_tick(void) {
    if (!bitfield_writable()) return;
    uint8_t *mem = (uint8_t *)(uintptr_t)MTF_UNLOCK_BITFIELD_VA;
    if (!g_snap_taken) {
        memcpy(g_orig, mem, MTF_UNLOCK_BITFIELD_SIZE);
        g_snap_taken = 1;
    }
    for (int i = 0; i < CHEAT_COUNT; i++) {
        int want = cfg_bool(kCheatKeys[i]) ? 1 : 0;
        if (want) {
            fs_save_profile_mark_tainted();
            achievements_notify_casual("Cheats enabled");
            apply_category(mem, i, 1);
        } else if (g_prev_state[i]) {
            apply_category(mem, i, 0);
        }
        if (want != g_prev_state[i]) {
            rt_log("cheats: %s -> %s", kCheatKeys[i], want ? "on" : "off");
            g_prev_state[i] = want;
        }
    }
}

void cheats_set_all(int enable) {
    const char *v = enable ? "1" : "0";
    for (int i = 0; i < CHEAT_COUNT; i++) {
        cfg_set(kCheatKeys[i], v);
    }
}
