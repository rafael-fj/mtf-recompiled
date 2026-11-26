#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UPDATE_STATE_IDLE = 0,
    UPDATE_STATE_DOWNLOADING = 1,
    UPDATE_STATE_APPLYING = 2,
    UPDATE_STATE_ERROR = 3,
};

typedef struct {
    int available;
    char tag[32];
    char url[256];
    char download_url[512];
    uint64_t bytes_done;
    uint64_t bytes_total;
    int state;
    char error[128];
} UpdateInfo;

// Starts the background GitHub release check once (no-op if MTF_UPDATE_CHECK=0).
void update_check_start(void);

// Copies the current update status into *out; returns out->available (0 or 1).
int update_check_status(UpdateInfo *out);

// Opens the latest release page in the user's default browser.
void update_check_open_url(void);

// Returns 1 if an in-app self-update is supported (Windows asset URL + mtf-updater.exe present).
int update_check_can_install(void);

// Starts downloading the release archive and launches mtf-updater.exe when ready.
void update_check_start_install(void);

#ifdef __cplusplus
}
#endif
