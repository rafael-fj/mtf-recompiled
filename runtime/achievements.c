#include "achievements.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "../profiles/0400D4201C6EA000/cheats_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int g_sock = -1;
#endif

static int g_tried = 0;
static int g_connected = 0;
static int g_casual = 0;
static int g_save_hydrated = 0;
static int g_save_loaded = 0;
static uint64_t g_settle_until = 0;
static uint8_t g_prev_bits[MTF_UNLOCK_BITFIELD_SIZE];
static uint32_t g_sent_mask = 0;

enum {
    ACH_FIRST_CHAR = 0,
    ACH_ALL_CHARS,
    ACH_FIRST_COSTUME,
    ACH_FIRST_RACKET,
    ACH_ALL_RACKETS,
    ACH_FIRST_COURT,
    ACH_ALL_COURTS,
    ACH_FIRST_MODE,
    ACH_ALL_MODES,
    ACH_EVERYTHING,
};

#ifndef MTF_PORT_VERSION
#define MTF_PORT_VERSION "1.0.0"
#endif

static void pipe_write(const char *line) {
    if (!g_connected || !line) return;
    size_t n = strlen(line);
#ifdef _WIN32
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        if (!WriteFile(g_pipe, line, (DWORD)n, &wr, NULL)) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
            g_connected = 0;
        }
    }
#else
    if (g_sock >= 0) {
        if (send(g_sock, line, n, 0) < 0) {
            close(g_sock);
            g_sock = -1;
            g_connected = 0;
        }
    }
#endif
}

static void pipe_connect_once(void) {
    if (g_tried) return;
    g_tried = 1;
    const char *sess = getenv("RL_SESSION");
    if (!sess || !*sess) return;
#ifdef _WIN32
    char path[256];
    if (!strncmp(sess, "\\\\.\\pipe\\", 9))
        snprintf(path, sizeof path, "%s", sess);
    else
        snprintf(path, sizeof path, "\\\\.\\pipe\\RecompLauncher-%s", sess);
    for (int attempt = 0; attempt < 3; attempt++) {
        g_pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(path, 500);
        else break;
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    g_connected = 1;
#else
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (sess[0] == '/')
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sess);
    else
        snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/RecompLauncher-%s.sock", sess);
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0) return;
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(g_sock);
        g_sock = -1;
        return;
    }
    g_connected = 1;
#endif
    char hello[256];
    snprintf(hello, sizeof hello,
             "{\"t\":\"hello\",\"sdk\":1,\"console\":\"switch2\",\"memory\":false,\"game\":\"mtf-recompiled\",\"version\":\"%s\"}\n",
             MTF_PORT_VERSION);
    pipe_write(hello);
    rt_log("achievements: connected to RL_SESSION");
}

static int any_cheat_cfg_on(void) {
    return cfg_bool("MTF_CHEAT_UNLOCK_CHARS") || cfg_bool("MTF_CHEAT_UNLOCK_COSTUMES") ||
           cfg_bool("MTF_CHEAT_UNLOCK_RACKETS") || cfg_bool("MTF_CHEAT_UNLOCK_COURTS") ||
           cfg_bool("MTF_CHEAT_UNLOCK_MODES");
}

void achievements_notify_casual(const char *reason) {
    if (!g_casual) {
        g_casual = 1;
        rt_log("achievements: switched to casual (%s)", reason ? reason : "cheats");
    }
    pipe_connect_once();
    if (!g_connected) return;
    char msg[256];
    snprintf(msg, sizeof msg, "{\"t\":\"casual\",\"reason\":\"%s\"}\n", reason ? reason : "Cheats enabled");
    pipe_write(msg);
}

int achievements_connected(void) { return g_connected; }
int achievements_is_casual(void) { return g_casual || fs_save_profile_is_tainted(NULL) || any_cheat_cfg_on(); }

static void emit_event(int bit_id, const char *name, uint64_t frame) {
    if (g_sent_mask & (1u << bit_id)) return;
    g_sent_mask |= (1u << bit_id);
    rt_log("achievements: event '%s' (frame %llu)", name, (unsigned long long)frame);
    if (!g_connected || achievements_is_casual()) return;
    char msg[256];
    snprintf(msg, sizeof msg, "{\"t\":\"event\",\"name\":\"%s\",\"frame\":%llu}\n", name, (unsigned long long)frame);
    pipe_write(msg);
}

static void count_ranges(const uint8_t *mem, const CheatBitRange *r, size_t nr, int *set_out, int *tot_out) {
    int s = 0, t = 0;
    for (size_t i = 0; i < nr; i++) {
        for (uint16_t b = r[i].bit_start; b <= r[i].bit_end; b++) {
            uint16_t byte_idx = b >> 3;
            uint8_t mask = (uint8_t)(0x80u >> (b & 7u));
            if (byte_idx >= MTF_UNLOCK_BITFIELD_SIZE) continue;
            t++;
            if (mem[byte_idx] & mask) s++;
        }
    }
    *set_out = s;
    *tot_out = t;
}

static int bitfield_ready(void) {
    uint64_t end = 0;
    int st = plat_mem_state(MTF_UNLOCK_BITFIELD_VA, MTF_UNLOCK_BITFIELD_VA + MTF_UNLOCK_BITFIELD_SIZE, &end);
    if (st != PLAT_PROT_RW || end < MTF_UNLOCK_BITFIELD_VA + MTF_UNLOCK_BITFIELD_SIZE) return 0;
    return plat_mem_readable(MTF_UNLOCK_BITFIELD_VA, MTF_UNLOCK_BITFIELD_SIZE);
}

static int has_any_bit(const uint8_t *mem) {
    for (size_t i = 0; i < MTF_UNLOCK_BITFIELD_SIZE; i++)
        if (mem[i]) return 1;
    return 0;
}

void achievements_tick(void) {
    pipe_connect_once();
    if (fs_save_profile_is_tainted(NULL) || any_cheat_cfg_on()) {
        if (!g_casual) achievements_notify_casual("Cheats used on save profile");
        return;
    }
    if (!bitfield_ready()) return;

    static uint64_t g_tick = 0;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)MTF_UNLOCK_BITFIELD_VA;
    uint64_t tick = ++g_tick;
    if (!g_save_hydrated) {
        if (!has_any_bit(mem)) return;
        g_save_hydrated = 1;
        g_settle_until = tick + 60;
        memcpy(g_prev_bits, mem, MTF_UNLOCK_BITFIELD_SIZE);
        return;
    }

    if (!g_save_loaded) {
        memcpy(g_prev_bits, mem, MTF_UNLOCK_BITFIELD_SIZE);
        if (tick < g_settle_until) return;
        g_save_loaded = 1;
        if (g_connected) {
            char msg[128];
            snprintf(msg, sizeof msg, "{\"t\":\"save_loaded\",\"frame\":%llu}\n", (unsigned long long)tick);
            pipe_write(msg);
        }
        return;
    }

    if (memcmp(g_prev_bits, mem, MTF_UNLOCK_BITFIELD_SIZE) == 0) return;

    int ch_prev = 0, ch_now = 0, ch_tot = 0;
    int co_prev = 0, co_now = 0, co_tot = 0;
    int rk_prev = 0, rk_now = 0, rk_tot = 0;
    int ct_prev = 0, ct_now = 0, ct_tot = 0;
    int md_prev = 0, md_now = 0, md_tot = 0;

    count_ranges(g_prev_bits, kCheatCharRanges, sizeof kCheatCharRanges / sizeof *kCheatCharRanges, &ch_prev, &ch_tot);
    count_ranges(mem,         kCheatCharRanges, sizeof kCheatCharRanges / sizeof *kCheatCharRanges, &ch_now,  &ch_tot);
    count_ranges(g_prev_bits, kCheatCostumeRanges, sizeof kCheatCostumeRanges / sizeof *kCheatCostumeRanges, &co_prev, &co_tot);
    count_ranges(mem,         kCheatCostumeRanges, sizeof kCheatCostumeRanges / sizeof *kCheatCostumeRanges, &co_now,  &co_tot);
    count_ranges(g_prev_bits, kCheatRacketRanges, sizeof kCheatRacketRanges / sizeof *kCheatRacketRanges, &rk_prev, &rk_tot);
    count_ranges(mem,         kCheatRacketRanges, sizeof kCheatRacketRanges / sizeof *kCheatRacketRanges, &rk_now,  &rk_tot);
    count_ranges(g_prev_bits, kCheatCourtRanges, sizeof kCheatCourtRanges / sizeof *kCheatCourtRanges, &ct_prev, &ct_tot);
    count_ranges(mem,         kCheatCourtRanges, sizeof kCheatCourtRanges / sizeof *kCheatCourtRanges, &ct_now,  &ct_tot);
    count_ranges(g_prev_bits, kCheatModeRanges, sizeof kCheatModeRanges / sizeof *kCheatModeRanges, &md_prev, &md_tot);
    count_ranges(mem,         kCheatModeRanges, sizeof kCheatModeRanges / sizeof *kCheatModeRanges, &md_now,  &md_tot);

    if (ch_now > ch_prev) emit_event(ACH_FIRST_CHAR, "unlock_first_char", tick);
    if (ch_tot > 0 && ch_now == ch_tot && ch_prev < ch_tot) emit_event(ACH_ALL_CHARS, "unlock_all_chars", tick);
    if (co_now > co_prev) emit_event(ACH_FIRST_COSTUME, "unlock_first_costume", tick);
    if (rk_now > rk_prev) emit_event(ACH_FIRST_RACKET, "unlock_first_racket", tick);
    if (rk_tot > 0 && rk_now == rk_tot && rk_prev < rk_tot) emit_event(ACH_ALL_RACKETS, "unlock_all_rackets", tick);
    if (ct_now > ct_prev) emit_event(ACH_FIRST_COURT, "unlock_first_court", tick);
    if (ct_tot > 0 && ct_now == ct_tot && ct_prev < ct_tot) emit_event(ACH_ALL_COURTS, "unlock_all_courts", tick);
    if (md_now > md_prev) emit_event(ACH_FIRST_MODE, "unlock_first_mode", tick);
    if (md_tot > 0 && md_now == md_tot && md_prev < md_tot) emit_event(ACH_ALL_MODES, "unlock_all_modes", tick);
    if (ch_now == ch_tot && rk_now == rk_tot && ct_now == ct_tot && md_now == md_tot &&
        (ch_prev < ch_tot || rk_prev < rk_tot || ct_prev < ct_tot || md_prev < md_tot))
        emit_event(ACH_EVERYTHING, "unlock_everything", tick);

    memcpy(g_prev_bits, mem, MTF_UNLOCK_BITFIELD_SIZE);
}
