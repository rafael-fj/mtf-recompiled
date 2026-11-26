// Temporary Linux stub for browser streaming (Phase L3 will port stream_host/hwenc/webrtc).
#include "stream_host.h"
#include <string.h>

int stream_host_has_cloudflared(void) { return 0; }
int stream_host_install_cloudflared(void) { return 0; }
int stream_host_start_mode(int use_cloudflared) { (void)use_cloudflared; return 0; }
int stream_host_start(void) { return 0; }
void stream_host_stop(void) {}
int stream_copy_clipboard(const char *text) { (void)text; return 0; }
int stream_host_active(void) { return 0; }
void stream_host_info(StreamHostInfo *out) { if (out) memset(out, 0, sizeof *out); }
void stream_host_target_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
void stream_host_codec_info(int *hw_mask, int *active_codec) { if (hw_mask) *hw_mask = 0; if (active_codec) *active_codec = 0; }
void stream_host_push_video(const uint8_t *bgra, int w, int h, int stride) { (void)bgra; (void)w; (void)h; (void)stride; }
void stream_host_push_audio(const float *pcm, uint32_t frames, int ch, int rate) { (void)pcm; (void)frames; (void)ch; (void)rate; }
int stream_host_remote_connected(int player) { (void)player; return 0; }
int stream_host_remote_poll(int player, uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)player; (void)b; (void)lx; (void)ly; (void)rx; (void)ry;
    return 0;
}
int stream_host_pop_toast(int *player, int *connected) { (void)player; (void)connected; return 0; }
