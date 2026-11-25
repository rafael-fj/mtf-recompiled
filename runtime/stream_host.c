// Clean-room browser streaming host: embedded HTTP + WebSocket (RFC 6455) + libvpx VP8 + Opus + cloudflared tunnel.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "stream_host.h"
#include "stream_webrtc.h"
#include "stream_hwenc.h"

#define STREAM_PORT_DEFAULT 11455
#define STREAM_MAX_CLIENTS 3   // Players 2..4 (indices 1..3)
#define STREAM_VIDEO_MAX_W 960
#define STREAM_VIDEO_MAX_H 540
#define OPUS_FRAME_SAMPLES 960 // 20 ms @ 48 kHz

typedef struct {
    SOCKET sock;
    int active;
    int player_idx; // 1..3
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint64_t last_input_ns;
    int need_keyframe;
    int caps_known;
    int caps_av1;
    int caps_h264;
} StreamClient;

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PlatMutex g_send_lock = PLAT_MUTEX_INIT;

typedef struct { int player, connected; } StreamToast;
static StreamToast g_toast_q[16];
static unsigned g_toast_head = 0, g_toast_tail = 0;

static void push_toast(int player, int connected) {
    plat_mutex_lock(&g_lock);
    unsigned next = (g_toast_tail + 1) % 16;
    if (next != g_toast_head) {
        g_toast_q[g_toast_tail] = (StreamToast){player, connected};
        g_toast_tail = next;
    }
    plat_mutex_unlock(&g_lock);
}

int stream_host_pop_toast(int *player, int *connected) {
    plat_mutex_lock(&g_lock);
    if (g_toast_head == g_toast_tail) {
        plat_mutex_unlock(&g_lock);
        return 0;
    }
    StreamToast t = g_toast_q[g_toast_head];
    g_toast_head = (g_toast_head + 1) % 16;
    plat_mutex_unlock(&g_lock);
    if (player) *player = t.player;
    if (connected) *connected = t.connected;
    return 1;
}
static volatile int g_running = 0;
static volatile int g_state = STREAM_STATE_OFF;
static volatile int g_mode = STREAM_MODE_CLOUDFLARE;
static volatile int g_use_cf = 0;
static SOCKET g_listen = INVALID_SOCKET;
static int g_port = STREAM_PORT_DEFAULT;
static char g_code[16];
static char g_local_url[128];
static char g_public_url[256];
static char g_status_msg[160];
static uint64_t g_frames_sent = 0;
static uint64_t g_bytes_sent = 0;

static StreamClient g_clients[STREAM_MAX_CLIENTS];

// Child cloudflared process + job object (auto-killed when host exits)
static HANDLE g_cf_proc = NULL;
static HANDLE g_cf_job = NULL;
static HANDLE g_cf_pipe_r = NULL;
static HANDLE g_srv_thread = NULL;
static HANDLE g_cf_thread = NULL;
static int g_atexit_registered = 0;
static volatile int g_cf_installing = 0;
static char g_cf_install_msg[128] = {0};

// Video encoder state (protected by g_enc_lock)
static PlatMutex g_enc_lock = PLAT_MUTEX_INIT;
static vpx_codec_ctx_t g_vpx;
static int g_vpx_ok = 0;
static int g_vpx_w = 0, g_vpx_h = 0, g_vpx_quality = -1;
static vpx_image_t g_vpx_img;
static uint64_t g_vpx_pts = 0;
static int g_hw_fail_mask = 0;
static int g_hw_ok = 0, g_hw_codec = 0, g_hw_w = 0, g_hw_h = 0, g_hw_q = -1;
static int g_active_codec = STREAM_CODEC_VP8;
static uint8_t *g_nv12_buf = NULL;
static size_t g_nv12_cap = 0;

static int stream_quality_mode(void) {
    const char *v = cfg_value("MTF_STREAM_QUALITY");
    if (!strcmp(v, "performance")) return 0;
    if (!strcmp(v, "quality")) return 2;
    return 1;
}

void stream_host_target_size(int *w, int *h) {
    int q = stream_quality_mode();
    if (q == 0) {
        if (w) *w = 960;
        if (h) *h = 540;
    } else {
        if (w) *w = 1280;
        if (h) *h = 720;
    }
}
static uint64_t g_last_vid_ns = 0;

// Audio encoder state
static PlatMutex g_aud_lock = PLAT_MUTEX_INIT;
static OpusEncoder *g_opus = NULL;
static int g_opus_rate = 0;
static int16_t g_aud_buf[OPUS_FRAME_SAMPLES * 2];
static int g_aud_fill = 0;

// ---- SHA-1 + Base64 for RFC 6455 WebSocket handshake
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_digest(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t padded = ((len + 8) / 64 + 1) * 64;
    uint8_t *buf = calloc(1, padded);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + i * 4] << 24) | ((uint32_t)buf[off + i * 4 + 1] << 16) |
                   ((uint32_t)buf[off + i * 4 + 2] << 8) | buf[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < len ? ((uint32_t)in[i + 1] << 8) : 0) |
                     (i + 2 < len ? in[i + 2] : 0);
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[j] = 0;
}

static int sock_send_all(SOCKET s, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = send(s, (const char *)(buf + sent), (int)(len - sent), 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static void ws_send_frame(SOCKET s, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(((uint64_t)len >> ((7 - i) * 8)) & 0xff);
        hlen = 10;
    }
    plat_mutex_lock(&g_send_lock);
    if (s != INVALID_SOCKET) {
        sock_send_all(s, hdr, hlen);
        if (len) sock_send_all(s, payload, len);
    }
    plat_mutex_unlock(&g_send_lock);
}

static int ws_broadcast_bin(const uint8_t *payload, size_t len) {
    int dropped = 0;
    int need_ws = 0;
    if (g_mode == STREAM_MODE_WEBRTC) {
        dropped = webrtc_broadcast_media(payload, len);
        int any_rtc = 0;
        for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
            if (!g_clients[i].active) continue;
            if (webrtc_peer_is_open(i)) any_rtc = 1;
            else if (g_clients[i].sock != INVALID_SOCKET) need_ws = 1;
        }
        if (any_rtc) g_bytes_sent += len;
        if (!need_ws) return dropped;
    }
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[0] = 0x82; // FIN + Binary
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(((uint64_t)len >> ((7 - i) * 8)) & 0xff);
        hlen = 10;
    }
    int is_video_delta = (len >= 2 && payload[0] == 1 && (payload[1] & 1) == 0);
    plat_mutex_lock(&g_send_lock);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (!g_clients[i].active || g_clients[i].sock == INVALID_SOCKET) continue;
        if (g_mode == STREAM_MODE_WEBRTC && webrtc_peer_is_open(i)) continue;
        if (is_video_delta) {
            fd_set wfds;
            struct timeval tv = {0, 0};
            FD_ZERO(&wfds);
            FD_SET(g_clients[i].sock, &wfds);
            if (select(0, NULL, &wfds, NULL, &tv) <= 0 || !FD_ISSET(g_clients[i].sock, &wfds)) {
                dropped = 1;
                continue;
            }
        }
        if (sock_send_all(g_clients[i].sock, hdr, hlen) < 0 || sock_send_all(g_clients[i].sock, payload, len) < 0) {
            closesocket(g_clients[i].sock);
            g_clients[i].sock = INVALID_SOCKET;
            g_clients[i].active = 0;
        } else {
            g_bytes_sent += hlen + len;
        }
    }
    plat_mutex_unlock(&g_send_lock);
    return dropped;
}

// ---- Embedded Browser Client HTML5 (WebCodecs VP8 + Opus + Gamepad API + Keyboard)
static const char k_html_page[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>MTF Recomp — Browser Stream</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif;-webkit-tap-highlight-color:transparent;-webkit-touch-callout:none;-webkit-user-select:none;user-select:none}"
"body{background:#0b0e14;color:#e6edf3;display:flex;flex-direction:column;height:100vh;height:100dvh;overflow:hidden;touch-action:none;user-select:none;-webkit-user-select:none}"
"#bar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:6px 12px;background:#131924;border-bottom:1px solid #263042;font-size:13px}"
"#bar button{background:#2563eb;color:#fff;border:0;border-radius:6px;padding:6px 10px;cursor:pointer;font-weight:600;font-size:12px}"
"#bar button.sec{background:#334155}"
"#stat{margin-left:auto;color:#94a3b8;font-size:12px}"
"#stage{flex:1;min-height:0;display:flex;align-items:center;justify-content:center;background:#000;position:relative;overflow:hidden;touch-action:none}"
"canvas{width:100%;height:100%;object-fit:contain;background:#000}"
"#hint{position:absolute;bottom:10px;left:12px;background:rgba(0,0,0,.65);padding:6px 10px;border-radius:6px;font-size:12px;color:#cbd5e1;pointer-events:none}"
"#touchUI{position:absolute;inset:0;pointer-events:none;display:none;z-index:10}"
"#tStick{position:absolute;left:22px;bottom:22px;width:136px;height:136px;border-radius:50%;background:rgba(255,255,255,.10);border:2px solid rgba(255,255,255,.28);pointer-events:auto;touch-action:none}"
"#tKnob{position:absolute;left:50%;top:50%;width:54px;height:54px;margin:-27px 0 0 -27px;border-radius:50%;background:rgba(255,255,255,.45);border:2px solid rgba(255,255,255,.8);pointer-events:none}"
"#tBtns{position:absolute;right:20px;bottom:20px;width:152px;height:152px;pointer-events:none}"
".tb{position:absolute;width:50px;height:50px;border-radius:50%;background:rgba(30,41,59,.68);border:2px solid rgba(255,255,255,.42);color:#fff;font-weight:700;font-size:16px;display:flex;align-items:center;justify-content:center;pointer-events:auto;touch-action:none}"
".tb.on{background:rgba(59,130,246,.85);border-color:#fff;transform:scale(.94)}"
"#tbX{left:51px;top:0}#tbY{left:0;top:51px}#tbA{right:0;top:51px}#tbB{left:51px;bottom:0}"
".tsys{position:absolute;top:14px;height:34px;min-width:48px;padding:0 12px;border-radius:17px;background:rgba(30,41,59,.65);border:1.5px solid rgba(255,255,255,.35);color:#fff;font-weight:700;font-size:14px;display:flex;align-items:center;justify-content:center;pointer-events:auto;touch-action:none}"
".tsys.on{background:rgba(59,130,246,.85);border-color:#fff}"
"#tbL{left:22px}#tbR{right:22px}#tbMin{left:50%;transform:translateX(-58px)}#tbPlus{left:50%;transform:translateX(10px)}"
"</style></head><body>"
"<div id='bar'>"
"<strong>MTF Stream</strong>"
"<button id='btnConn'>Reconnect</button>"
"<button id='btnCrop' class='sec'>View: Full</button>"
"<button id='btnAudio' class='sec'>Audio: Off</button>"
"<button id='btnTouch' class='sec'>Overlay Controls</button>"
"<button id='btnFs' class='sec'>Fullscreen</button>"
"<span id='stat'>Connecting...</span>"
"</div>"
"<div id='stage'><canvas id='cv' width='960' height='540'></canvas>"
"<div id='hint'>Controls: Gamepad (automatic) or Keyboard (WASD=Stick, J/K/U/I=A/B/Y/X, Q/E=L/R, Enter=+)</div>"
"<div id='touchUI'>"
"<div id='tStick'><div id='tKnob'></div></div>"
"<div id='tBtns'>"
"<div id='tbX' class='tb' data-bit='2'>X</div>"
"<div id='tbY' class='tb' data-bit='3'>Y</div>"
"<div id='tbA' class='tb' data-bit='0'>A</div>"
"<div id='tbB' class='tb' data-bit='1'>B</div>"
"</div>"
"<div id='tbL' class='tsys' data-bit='6'>L</div>"
"<div id='tbR' class='tsys' data-bit='7'>R</div>"
"<div id='tbMin' class='tsys' data-bit='11'>-</div>"
"<div id='tbPlus' class='tsys' data-bit='10'>+</div>"
"</div>"
"</div>"
"<script>"
"const qs=new URLSearchParams(location.search),roomCode=(qs.get('code')||'').toUpperCase();"
"const statEl=document.getElementById('stat'),cv=document.getElementById('cv'),ctx=cv.getContext('2d',{alpha:false,desynchronized:true});"
"let ws=null,pc=null,dcIn=null,vdec=null,vdecCodec=-1,adec=null,actx=null,audioOn=false,cropMode=0,nextPlayTime=0,needKf=true,useRtc=false;"
"const keys={};"
"window.addEventListener('keydown',e=>{keys[e.code]=1;if(['Space','ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.code))e.preventDefault();});"
"window.addEventListener('keyup',e=>{keys[e.code]=0;});"
"document.getElementById('btnCrop').onclick=()=>{cropMode=(cropMode+1)%3;"
"const names=['Full','P2 Right','P1 Left'];document.getElementById('btnCrop').textContent='View: '+names[cropMode];};"
"document.getElementById('btnFs').onclick=()=>{const st=document.getElementById('stage');"
"if(!document.fullscreenElement)(st.requestFullscreen||st.webkitRequestFullscreen).call(st);"
"else(document.exitFullscreen||document.webkitExitFullscreen).call(document);};"
"document.getElementById('btnAudio').onclick=async()=>{"
"if(!actx)actx=new AudioContext({sampleRate:48000,latencyHint:'interactive'});"
"if(actx.state==='suspended')await actx.resume();"
"audioOn=!audioOn;document.getElementById('btnAudio').textContent='Audio: '+(audioOn?'On':'Off');nextPlayTime=0;};"
"function codecStr(c){return c===2?'av01.0.04M.08':c===1?'avc1.42E01F':'vp8';}"
"function initVideoDecoder(c){"
"if(vdec)try{vdec.close()}catch(_){}"
"vdecCodec=c;needKf=true;"
"vdec=new VideoDecoder({output:f=>{"
"const vr=f.visibleRect||{x:0,y:0,width:f.displayWidth,height:f.displayHeight};"
"const sw=vr.width||f.displayWidth,sh=vr.height||f.displayHeight;"
"if(cropMode===0){if(cv.width!==sw||cv.height!==sh){cv.width=sw;cv.height=sh;}ctx.drawImage(f,vr.x,vr.y,sw,sh,0,0,sw,sh);}"
"else{const hw=(sw/2)|0,sx=vr.x+(cropMode===1?(sw-hw):0);if(cv.width!==hw||cv.height!==sh){cv.width=hw;cv.height=sh;}ctx.drawImage(f,sx,vr.y,hw,sh,0,0,hw,sh);}"
"f.close();},error:e=>{console.error('vdec',e);setTimeout(()=>{initVideoDecoder(vdecCodec);if(ws&&ws.readyState===1)ws.send('{\"type\":\"kf\"}');},50);}});"
"vdec.configure({codec:codecStr(c),optimizeForLatency:true,colorSpace:{primaries:'bt709',transfer:'iec61966-2-1',matrix:'bt470bg',fullRange:true}});"
"}"
"function initDecoders(){"
"initVideoDecoder(0);"
"if(adec)try{adec.close()}catch(_){}"
"if('AudioDecoder'in window){adec=new AudioDecoder({output:ad=>{"
"if(!audioOn||!actx){ad.close();return;}"
"const ch=ad.numberOfChannels,n=ad.numberOfFrames,buf=actx.createBuffer(ch,n,ad.sampleRate);"
"for(let c=0;c<ch;c++){const tmp=new Float32Array(n);ad.copyTo(tmp,{planeIndex:c,format:'f32-planar'});buf.copyToChannel(tmp,c);}"
"ad.close();const src=actx.createBufferSource();src.buffer=buf;src.connect(actx.destination);"
"const now=actx.currentTime;if(nextPlayTime<now||nextPlayTime>now+0.12)nextPlayTime=now+0.015;"
"src.start(nextPlayTime);nextPlayTime+=buf.duration;},error:e=>console.error('adec',e)});"
"adec.configure({codec:'opus',sampleRate:48000,numberOfChannels:2});}"
"}"
"function onMediaBuf(ab){"
"const u=new Uint8Array(ab),dv=new DataView(ab),t=u[0];"
"if(t===1){"
"const c=u[1]>>4,kf=(u[1]&1)===1,ts=dv.getUint32(6,true)*1000;"
"if(c!==vdecCodec)initVideoDecoder(c);"
"if(!vdec||vdec.state!=='configured')return;"
"if(needKf&&!kf)return; needKf=false;"
"vdec.decode(new EncodedVideoChunk({type:kf?'key':'delta',timestamp:ts,data:u.subarray(10)}));"
"}else if(t===2&&adec&&adec.state==='configured'&&audioOn){"
"const ts=dv.getUint32(4,true)*1000;"
"adec.decode(new EncodedAudioChunk({type:'key',timestamp:ts,data:u.subarray(8)}));"
"}else if(t===3){"
"useRtc=u.length>2&&u[2]===1;"
"statEl.textContent='Connected as Player '+(u[1]+1)+(useRtc?' (WebRTC UDP negotiating...)':' (WebSocket)');"
"if(useRtc)startRtc(u[1]+1);"
"}}"
"async function startRtc(pnum){"
"if(pc){try{pc.close()}catch(_){}}"
"dcIn=null;"
"pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});"
"pc.onicecandidate=e=>{if(e.candidate&&ws&&ws.readyState===1)"
"ws.send(JSON.stringify({type:'candidate',candidate:e.candidate.candidate,mid:e.candidate.sdpMid||'0'}));};"
"pc.onconnectionstatechange=()=>{if(pc.connectionState==='failed'||pc.connectionState==='closed')"
"{statEl.textContent='Connected as Player '+pnum+' (WebSocket fallback)';}};"
"pc.ondatachannel=ev=>{"
"const dc=ev.channel;dc.binaryType='arraybuffer';"
"if(dc.label==='media'){dc.onopen=()=>{statEl.textContent='Connected as Player '+pnum+' (WebRTC UDP)';};"
"dc.onmessage=m=>onMediaBuf(m.data);}"
"else if(dc.label==='input'){dcIn=dc;}"
"};}"
"function connect(){"
"if(pc){try{pc.close()}catch(_){} pc=null; dcIn=null;}"
"if(ws){ws.close();ws=null;}"
"if(!('VideoDecoder'in window)){statEl.textContent='WebCodecs not supported in this browser';return;}"
"initDecoders();useRtc=false;"
"const proto=location.protocol==='https:'?'wss://':'ws://';"
"const curWs=new WebSocket(proto+location.host+'/ws'+(roomCode?'?code='+encodeURIComponent(roomCode):''));"
"ws=curWs;ws.binaryType='arraybuffer';"
"ws.onopen=async()=>{statEl.textContent='Connected (waiting frames...)';"
"let av1=false,h264=false;"
"if(!/Firefox/i.test(navigator.userAgent))try{av1=(await VideoDecoder.isConfigSupported({codec:'av01.0.04M.08',optimizeForLatency:true})).supported;}catch(_){}"
"try{h264=(await VideoDecoder.isConfigSupported({codec:'avc1.42E01F',optimizeForLatency:true})).supported;}catch(_){}"
"if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'caps',av1,h264}));};"
"ws.onclose=e=>{if(ws!==curWs)return;statEl.textContent='Disconnected ('+(e.reason||e.code)+') - reconnecting...';setTimeout(()=>{if(ws===curWs)connect();},1500);};"
"ws.onmessage=async ev=>{"
"if(typeof ev.data==='string'){"
"const msg=JSON.parse(ev.data);"
"if(msg.type==='offer'&&pc){"
"await pc.setRemoteDescription({type:'offer',sdp:msg.sdp});"
"const ans=await pc.createAnswer();await pc.setLocalDescription(ans);"
"ws.send(JSON.stringify({type:'answer',sdp:ans.sdp}));"
"}else if(msg.type==='candidate'&&pc&&msg.candidate){"
"try{await pc.addIceCandidate({candidate:msg.candidate,sdpMid:msg.mid||'0'});}catch(_){}"
"}return;}"
"onMediaBuf(ev.data);"
"};}"
"document.getElementById('btnConn').onclick=connect;"
"let tBits=0,tTapBits=0,tTapTicks=0,tLx=0,tLy=0,stickPid=null;"
"const touchEl=document.getElementById('touchUI'),hintEl=document.getElementById('hint'),knobEl=document.getElementById('tKnob'),stickEl=document.getElementById('tStick'),btnTouch=document.getElementById('btnTouch');"
"const isCoarse=window.matchMedia('(pointer:coarse)').matches||('ontouchstart'in window&&navigator.maxTouchPoints>0);"
"let showTouch=isCoarse;"
"function syncTouchUI(){touchEl.style.display=showTouch?'block':'none';hintEl.style.display=isCoarse?'none':'block';"
"btnTouch.textContent='Overlay Controls: '+(showTouch?'On':'Off');}"
"btnTouch.onclick=()=>{showTouch=!showTouch;syncTouchUI();};"
"syncTouchUI();"
"window.addEventListener('contextmenu',e=>e.preventDefault());"
"touchEl.addEventListener('touchstart',e=>{if(e.target.closest('[data-bit],#tStick'))e.preventDefault();},{passive:false});"
"function updStick(cx,cy){const r=stickEl.getBoundingClientRect(),mx=r.left+r.width/2,my=r.top+r.height/2,rad=r.width*0.42;"
"let dx=cx-mx,dy=cy-my,d=Math.hypot(dx,dy);if(d>rad){dx=dx*rad/d;dy=dy*rad/d;}"
"knobEl.style.transform='translate('+dx.toFixed(1)+'px,'+dy.toFixed(1)+'px)';"
"tLx=((dx/rad)*32767)|0;tLy=((-dy/rad)*32767)|0;}"
"stickEl.addEventListener('pointerdown',e=>{e.preventDefault();stickPid=e.pointerId;stickEl.setPointerCapture(e.pointerId);updStick(e.clientX,e.clientY);sendInput();});"
"stickEl.addEventListener('pointermove',e=>{if(e.pointerId===stickPid){e.preventDefault();updStick(e.clientX,e.clientY);}});"
"function endStick(e){if(e.pointerId===stickPid){stickPid=null;tLx=0;tLy=0;knobEl.style.transform='translate(0,0)';sendInput();}}"
"stickEl.addEventListener('pointerup',endStick);stickEl.addEventListener('pointercancel',endStick);"
"document.querySelectorAll('#touchUI [data-bit]').forEach(el=>{const bit=1<<(+el.dataset.bit),pids=new Set();"
"el.addEventListener('pointerdown',e=>{e.preventDefault();pids.add(e.pointerId);el.setPointerCapture(e.pointerId);"
"tBits|=bit;tTapBits|=bit;tTapTicks=3;el.classList.add('on');sendInput();});"
"const up=e=>{if(pids.delete(e.pointerId)&&pids.size===0){tBits&=~bit;el.classList.remove('on');sendInput();}};"
"el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up);});"
"function readInput(){"
"let b=tBits|tTapBits,lx=tLx,ly=tLy,rx=0,ry=0;"
"if(tTapTicks>0){if(--tTapTicks===0)tTapBits=0;}"
"if(keys['KeyJ']||keys['KeyZ'])b|=1;"
"if(keys['KeyK']||keys['KeyX'])b|=2;"
"if(keys['KeyI']||keys['KeyC'])b|=4;"
"if(keys['KeyU']||keys['KeyV'])b|=8;"
"if(keys['KeyQ'])b|=1<<6; if(keys['KeyE'])b|=1<<7;"
"if(keys['Digit1'])b|=1<<8; if(keys['Digit3'])b|=1<<9;"
"if(keys['Enter'])b|=1<<10; if(keys['Backspace'])b|=1<<11;"
"if(keys['ArrowLeft'])b|=1<<12; if(keys['ArrowUp'])b|=1<<13;"
"if(keys['ArrowRight'])b|=1<<14; if(keys['ArrowDown'])b|=1<<15;"
"if(keys['KeyA'])lx=-32767; if(keys['KeyD'])lx=32767;"
"if(keys['KeyW'])ly=32767; if(keys['KeyS'])ly=-32767;"
"const gps=navigator.getGamepads?navigator.getGamepads():[];"
"for(const gp of gps){if(!gp)continue;"
"const map=[1,0,3,2,10,11,4,5,6,7,9,8,14,12,15,13];"
"for(let i=0;i<map.length;i++)if(gp.buttons[map[i]]&&gp.buttons[map[i]].pressed)b|=1<<i;"
"if(gp.axes.length>=2){const x=gp.axes[0],y=-gp.axes[1];if(Math.abs(x)>0.08)lx=(x*32767)|0;if(Math.abs(y)>0.08)ly=(y*32767)|0;}"
"if(gp.axes.length>=4){const x=gp.axes[2],y=-gp.axes[3];if(Math.abs(x)>0.08)rx=(x*32767)|0;if(Math.abs(y)>0.08)ry=(y*32767)|0;}"
"break;}"
"return [b,lx,ly,rx,ry];}"
"function sendInput(){"
"const [b,lx,ly,rx,ry]=readInput();"
"const buf=new ArrayBuffer(13),dv=new DataView(buf);"
"dv.setUint8(0,0x11);dv.setUint32(1,b>>>0,true);dv.setInt16(5,lx,true);dv.setInt16(7,ly,true);dv.setInt16(9,rx,true);dv.setInt16(11,ry,true);"
"if(dcIn&&dcIn.readyState==='open')dcIn.send(buf);"
"else if(ws&&ws.readyState===1)ws.send(buf);}"
"setInterval(sendInput,8);"
"setInterval(()=>{if(ws&&ws.readyState===1)ws.send('{\"type\":\"ping\"}');},25000);"
"connect();"
"</script></body></html>";

static int recv_exact(SOCKET s, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = recv(s, (char *)(buf + got), (int)(len - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void apply_input_pkt(int slot, const uint8_t *buf, size_t plen) {
    if (plen < 13 || buf[0] != 0x11) return;
    uint32_t b;
    int16_t lx, ly, rx, ry;
    memcpy(&b, buf + 1, 4);
    memcpy(&lx, buf + 5, 2);
    memcpy(&ly, buf + 7, 2);
    memcpy(&rx, buf + 9, 2);
    memcpy(&ry, buf + 11, 2);
    plat_mutex_lock(&g_lock);
    g_clients[slot].buttons = b;
    g_clients[slot].lx = lx;
    g_clients[slot].ly = ly;
    g_clients[slot].rx = rx;
    g_clients[slot].ry = ry;
    g_clients[slot].last_input_ns = plat_now_ns();
    plat_mutex_unlock(&g_lock);
}

static void on_rtc_signal(void *user, const char *json_msg) {
    int slot = (int)(intptr_t)user;
    if (slot < 0 || slot >= STREAM_MAX_CLIENTS || !json_msg) return;
    SOCKET s = g_clients[slot].sock;
    if (s != INVALID_SOCKET) ws_send_frame(s, 0x1, (const uint8_t *)json_msg, strlen(json_msg));
}

static void on_rtc_input(void *user, int slot, const uint8_t *data, size_t len) {
    (void)user;
    apply_input_pkt(slot, data, len);
}

static void on_rtc_state(void *user, int slot, int connected) {
    (void)user;
    if (connected) {
        plat_mutex_lock(&g_lock);
        g_clients[slot].need_keyframe = 1;
        g_clients[slot].last_input_ns = plat_now_ns();
        plat_mutex_unlock(&g_lock);
        rt_log("stream: Player %d WebRTC UDP DataChannel open", slot + 2);
    } else {
        rt_log("stream: Player %d WebRTC UDP closed", slot + 2);
    }
}

static DWORD WINAPI ws_client_thread(LPVOID param) {
    int slot = (int)(intptr_t)param;
    SOCKET s = g_clients[slot].sock;
    int is_rtc = (g_mode == STREAM_MODE_WEBRTC);
    uint8_t hello[3] = {0x03, (uint8_t)g_clients[slot].player_idx, (uint8_t)(is_rtc ? 1 : 0)};
    ws_send_frame(s, 0x2, hello, sizeof(hello));

    if (is_rtc) {
        webrtc_peer_start(slot, NULL, NULL, on_rtc_signal, (void *)(intptr_t)slot, on_rtc_input, on_rtc_state, NULL);
    }

    uint8_t *buf = malloc(8192);
    while (buf && g_running && g_clients[slot].active) {
        uint8_t h[2];
        if (recv_exact(s, h, 2) < 0) break;
        uint8_t opcode = h[0] & 0x0f;
        int masked = (h[1] & 0x80) != 0;
        uint64_t plen = h[1] & 0x7f;
        if (plen == 126) {
            uint8_t ext[2];
            if (recv_exact(s, ext, 2) < 0) break;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (recv_exact(s, ext, 8) < 0) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }
        uint8_t mask[4] = {0};
        if (masked && recv_exact(s, mask, 4) < 0) break;
        if (plen >= 8192) break;
        if (plen > 0 && recv_exact(s, buf, (size_t)plen) < 0) break;
        if (masked) {
            for (uint64_t i = 0; i < plen; i++) buf[i] ^= mask[i & 3];
        }
        buf[plen] = 0;
        if (opcode == 0x8) break; // Close
        if (opcode == 0x9) {      // Ping -> Pong
            ws_send_frame(s, 0xA, buf, (size_t)plen);
            continue;
        }
        if (opcode == 0x1) {
            if (strstr((const char *)buf, "\"caps\"")) {
                int av1 = strstr((const char *)buf, "\"av1\":true") != NULL;
                int h264 = strstr((const char *)buf, "\"h264\":true") != NULL;
                plat_mutex_lock(&g_lock);
                g_clients[slot].caps_known = 1;
                g_clients[slot].caps_av1 = av1;
                g_clients[slot].caps_h264 = h264;
                g_clients[slot].need_keyframe = 1;
                plat_mutex_unlock(&g_lock);
                rt_log("stream: Player %d browser decode caps: AV1=%s, H.264=%s, VP8=yes",
                       slot + 2, av1 ? "yes" : "no", h264 ? "yes" : "no");
            } else if (strstr((const char *)buf, "\"kf\"")) {
                plat_mutex_lock(&g_lock);
                g_clients[slot].need_keyframe = 1;
                plat_mutex_unlock(&g_lock);
            } else if (is_rtc && !strstr((const char *)buf, "\"ping\"")) {
                webrtc_peer_handle_signal(slot, (const char *)buf);
            }
            continue;
        }
        if ((opcode == 0x2 || opcode == 0x0) && plen >= 13 && buf[0] == 0x11) {
            apply_input_pkt(slot, buf, (size_t)plen);
        }
    }
    free(buf);
    if (is_rtc) webrtc_peer_close(slot);
    plat_mutex_lock(&g_lock);
    if (g_clients[slot].sock != INVALID_SOCKET) closesocket(g_clients[slot].sock);
    g_clients[slot].sock = INVALID_SOCKET;
    g_clients[slot].active = 0;
    g_clients[slot].buttons = 0;
    plat_mutex_unlock(&g_lock);
    push_toast(slot + 2, 0);
    rt_log("stream: Player %d disconnected from browser", slot + 2);
    return 0;
}

static const char *strcasestr_ci(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen) return hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, nlen) == 0) return hay;
    }
    return NULL;
}

static void handle_http_conn(SOCKET s) {
    DWORD rcv_to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_to, sizeof(rcv_to));
    char req[4096];
    int total = 0;
    while (total < (int)sizeof(req) - 1) {
        int n = recv(s, req + total, (int)sizeof(req) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        req[total] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (total <= 0) { closesocket(s); return; }
    rcv_to = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_to, sizeof(rcv_to));
    DWORD snd_to = 1500;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd_to, sizeof(snd_to));
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    if (strcasestr_ci(req, "Upgrade: websocket")) {
        // In Local Host mode, verify ?code=; in Cloudflare mode, the random tunnel URL itself is the secret token.
        if (!g_use_cf && g_code[0]) {
            char code_param[32] = {0};
            const char *q = strstr(req, "code=");
            if (q) {
                q += 5;
                size_t i = 0;
                while (q[i] && q[i] != '&' && q[i] != ' ' && i < sizeof(code_param) - 1) {
                    code_param[i] = q[i];
                    i++;
                }
            }
            if (_stricmp(code_param, g_code) != 0) {
                const char *unauth = "HTTP/1.1 403 Forbidden\r\nContent-Length: 12\r\nConnection: close\r\n\r\nInvalid code";
                send(s, unauth, (int)strlen(unauth), 0);
                closesocket(s);
                return;
            }
        }
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
        const char *k = strcasestr_ci(req, "Sec-WebSocket-Key:");
        if (!k) { closesocket(s); return; }
        k += 18;
        while (*k == ' ' || *k == '\t') k++;
        char key[128] = {0};
        size_t ki = 0;
        while (k[ki] && k[ki] != '\r' && k[ki] != '\n' && ki < 64) { key[ki] = k[ki]; ki++; }
        strcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        uint8_t digest[20];
        sha1_digest((const uint8_t *)key, strlen(key), digest);
        char accept_b64[64];
        base64_encode(digest, 20, accept_b64);

        plat_mutex_lock(&g_lock);
        int slot = -1;
        for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
            if (!g_clients[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            plat_mutex_unlock(&g_lock);
            const char *full = "HTTP/1.1 503 Room Full\r\nContent-Length: 9\r\nConnection: close\r\n\r\nRoom full";
            send(s, full, (int)strlen(full), 0);
            closesocket(s);
            return;
        }
        char resp[512];
        int rl = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept_b64);
        sock_send_all(s, (const uint8_t *)resp, (size_t)rl);
        memset(&g_clients[slot], 0, sizeof(g_clients[slot]));
        g_clients[slot].sock = s;
        g_clients[slot].active = 1;
        g_clients[slot].player_idx = slot + 1;
        g_clients[slot].need_keyframe = 1;
        g_clients[slot].last_input_ns = plat_now_ns();
        plat_mutex_unlock(&g_lock);
        push_toast(slot + 2, 1);
        rt_log("stream: browser connected as Player %d", slot + 2);
        HANDLE th = CreateThread(NULL, 0, ws_client_thread, (LPVOID)(intptr_t)slot, 0, NULL);
        if (th) CloseHandle(th);
        return;
    }

    // Normal HTTP GET -> serve embedded HTML5 player
    char hdr[256];
    size_t blen = sizeof(k_html_page) - 1;
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      blen);
    sock_send_all(s, (const uint8_t *)hdr, (size_t)hl);
    sock_send_all(s, (const uint8_t *)k_html_page, blen);
    closesocket(s);
}

static DWORD WINAPI server_thread_fn(LPVOID arg) {
    (void)arg;
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen, &fds);
        struct timeval tv = {0, 100000};
        int r = select(0, &fds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(g_listen, &fds)) {
            SOCKET c = accept(g_listen, NULL, NULL);
            if (c != INVALID_SOCKET) handle_http_conn(c);
        }
    }
    return 0;
}

static int find_cloudflared(char *out, size_t cap) {
    if (SearchPathA(NULL, "cloudflared.exe", NULL, (DWORD)cap, out, NULL) > 0) return 1;
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) > 0) {
        char *sl = strrchr(exe, '\\');
        if (sl) {
            *sl = 0;
            snprintf(out, cap, "%s\\cloudflared.exe", exe);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    const char *cands[] = {
        "toolchain\\cloudflared.exe",
        "C:\\Program Files\\cloudflared\\cloudflared.exe",
        "C:\\Program Files (x86)\\cloudflared\\cloudflared.exe",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(*cands); i++) {
        if (GetFileAttributesA(cands[i]) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", cands[i]);
            return 1;
        }
    }
    return 0;
}

int stream_host_has_cloudflared(void) {
    char tmp[MAX_PATH];
    return find_cloudflared(tmp, sizeof(tmp));
}

static DWORD WINAPI cf_install_thread(LPVOID arg) {
    (void)arg;
    char exe[MAX_PATH], dst[MAX_PATH], tmp[MAX_PATH], cmd[1024];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) {
        plat_mutex_lock(&g_lock);
        snprintf(g_cf_install_msg, sizeof(g_cf_install_msg), "Failed to resolve executable path");
        g_cf_installing = 0;
        plat_mutex_unlock(&g_lock);
        return 1;
    }
    char *sl = strrchr(exe, '\\');
    if (sl) *sl = 0;
    snprintf(dst, sizeof(dst), "%s\\cloudflared.exe", exe);
    snprintf(tmp, sizeof(tmp), "%s\\cloudflared.exe.tmp", exe);
    DeleteFileA(tmp);

    snprintf(cmd, sizeof(cmd),
             "curl.exe -fsSL -L --retry 2 -o \"%s\" "
             "\"https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe\"",
             tmp);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD exit_code = 1;
    if (ok) {
        WaitForSingleObject(pi.hProcess, 180000);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (ok && exit_code == 0 && GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES &&
        MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
        plat_mutex_lock(&g_lock);
        snprintf(g_cf_install_msg, sizeof(g_cf_install_msg), "Installed cloudflared.exe");
        g_cf_installing = 0;
        plat_mutex_unlock(&g_lock);
        rt_log("stream: installed cloudflared.exe to %s", dst);
        return 0;
    }
    DeleteFileA(tmp);
    plat_mutex_lock(&g_lock);
    snprintf(g_cf_install_msg, sizeof(g_cf_install_msg),
             "Download failed (exit %lu). Check internet or place cloudflared.exe next to mtf-recomp.exe",
             (unsigned long)exit_code);
    g_cf_installing = 0;
    plat_mutex_unlock(&g_lock);
    rt_log("stream: cloudflared.exe download failed (ok=%d exit=%lu)", (int)ok, (unsigned long)exit_code);
    return 1;
}

int stream_host_install_cloudflared(void) {
    plat_mutex_lock(&g_lock);
    if (g_cf_installing) { plat_mutex_unlock(&g_lock); return 0; }
    g_cf_installing = 1;
    snprintf(g_cf_install_msg, sizeof(g_cf_install_msg), "Downloading cloudflared.exe...");
    plat_mutex_unlock(&g_lock);
    HANDLE th = CreateThread(NULL, 0, cf_install_thread, NULL, 0, NULL);
    if (!th) {
        plat_mutex_lock(&g_lock);
        g_cf_installing = 0;
        snprintf(g_cf_install_msg, sizeof(g_cf_install_msg), "Failed to start installer thread");
        plat_mutex_unlock(&g_lock);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

static DWORD WINAPI cf_reader_thread(LPVOID arg) {
    (void)arg;
    char buf[512];
    DWORD nr = 0;
    char line[1024];
    size_t li = 0;
    while (g_running && g_cf_pipe_r && ReadFile(g_cf_pipe_r, buf, sizeof(buf) - 1, &nr, NULL) && nr > 0) {
        for (DWORD i = 0; i < nr; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r' || li >= sizeof(line) - 1) {
                line[li] = 0;
                const char *u = strstr(line, "https://");
                if (u) {
                    const char *end = strstr(u, ".trycloudflare.com");
                    if (end) {
                        size_t ulen = (size_t)(end + 18 - u);
                        if (ulen < 200) {
                            char base[210];
                            memcpy(base, u, ulen);
                            base[ulen] = 0;
                            plat_mutex_lock(&g_lock);
                            snprintf(g_public_url, sizeof(g_public_url), "%s", base);
                            g_state = STREAM_STATE_LIVE;
                            snprintf(g_status_msg, sizeof(g_status_msg), "Cloudflare tunnel online");
                            plat_mutex_unlock(&g_lock);
                            rt_log("stream: Cloudflare link ready -> %s", g_public_url);
                        }
                    }
                }
                li = 0;
            } else {
                line[li++] = ch;
            }
        }
    }
    return 0;
}

static void start_cloudflared(int port) {
    char cf_exe[MAX_PATH];
    if (!find_cloudflared(cf_exe, sizeof(cf_exe))) {
        plat_mutex_lock(&g_lock);
        g_state = STREAM_STATE_LOCAL_ONLY;
        snprintf(g_status_msg, sizeof(g_status_msg), "cloudflared.exe not found (local URL active)");
        plat_mutex_unlock(&g_lock);
        rt_log("stream: cloudflared.exe not found; local stream only at %s", g_local_url);
        return;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE pr = NULL, pw = NULL;
    if (!CreatePipe(&pr, &pw, &sa, 0)) return;
    SetHandleInformation(pr, HANDLE_FLAG_INHERIT, 0);

    g_cf_job = CreateJobObjectA(NULL, NULL);
    if (g_cf_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_cf_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "\"%s\" tunnel --url http://127.0.0.1:%d --no-autoupdate", cf_exe, port);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pw;
    si.hStdError = pw;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    if (!ok) ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(pw);
    if (!ok) {
        CloseHandle(pr);
        if (g_cf_job) { CloseHandle(g_cf_job); g_cf_job = NULL; }
        plat_mutex_lock(&g_lock);
        g_state = STREAM_STATE_LOCAL_ONLY;
        snprintf(g_status_msg, sizeof(g_status_msg), "Failed to launch cloudflared.exe");
        plat_mutex_unlock(&g_lock);
        return;
    }
    if (g_cf_job) AssignProcessToJobObject(g_cf_job, pi.hProcess);
    CloseHandle(pi.hThread);
    g_cf_proc = pi.hProcess;
    g_cf_pipe_r = pr;
    g_cf_thread = CreateThread(NULL, 0, cf_reader_thread, NULL, 0, NULL);
}

int stream_host_start_mode(int mode) {
    int want_cf = (mode == STREAM_MODE_CLOUDFLARE) || (mode == STREAM_MODE_WEBRTC && stream_host_has_cloudflared());
    plat_mutex_lock(&g_lock);
    if (g_running) { plat_mutex_unlock(&g_lock); return 1; }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_state = STREAM_STATE_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg), "WSAStartup failed");
        plat_mutex_unlock(&g_lock);
        return 0;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        g_state = STREAM_STATE_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg), "Socket creation failed");
        plat_mutex_unlock(&g_lock);
        return 0;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(want_cf ? INADDR_LOOPBACK : INADDR_ANY);
    sa.sin_port = htons(STREAM_PORT_DEFAULT);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        sa.sin_port = 0; // ephemeral fallback
        if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            closesocket(ls);
            g_state = STREAM_STATE_ERROR;
            snprintf(g_status_msg, sizeof(g_status_msg), "Bind failed");
            plat_mutex_unlock(&g_lock);
            return 0;
        }
    }
    socklen_t slen = sizeof(sa);
    getsockname(ls, (struct sockaddr *)&sa, &slen);
    g_port = ntohs(sa.sin_port);
    listen(ls, 8);
    g_listen = ls;

    uint8_t rnd[6];
    BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 6; i++) g_code[i] = alpha[rnd[i] % (sizeof(alpha) - 1)];
    g_code[6] = 0;

    snprintf(g_local_url, sizeof(g_local_url), "http://127.0.0.1:%d/?code=%s", g_port, g_code);
    g_public_url[0] = 0;
    g_mode = mode;
    g_use_cf = want_cf ? 1 : 0;
    if (want_cf) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 mode == STREAM_MODE_WEBRTC ? "Starting WebRTC signaling tunnel..." : "Starting Cloudflare tunnel...");
        g_state = STREAM_STATE_STARTING;
    } else {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 mode == STREAM_MODE_WEBRTC ? "WebRTC local signaling active" : "Local Host active");
        g_state = STREAM_STATE_LOCAL_ONLY;
    }
    g_frames_sent = 0;
    g_bytes_sent = 0;
    memset(g_clients, 0, sizeof(g_clients));
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) g_clients[i].sock = INVALID_SOCKET;
    g_running = 1;
    if (!g_atexit_registered) { atexit(stream_host_stop); g_atexit_registered = 1; }
    plat_mutex_unlock(&g_lock);

    g_srv_thread = CreateThread(NULL, 0, server_thread_fn, NULL, 0, NULL);
    if (want_cf) start_cloudflared(g_port);
    rt_log("stream: server listening on port %d (mode=%d cf=%d)", g_port, mode, want_cf);
    return 1;
}

int stream_host_start(void) { return stream_host_start_mode(1); }

int stream_copy_clipboard(const char *text) {
    if (!text || !*text) return 0;
    size_t len = strlen(text) + 1;
    if (!OpenClipboard(NULL)) return 0;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hg) { CloseClipboard(); return 0; }
    void *dst = GlobalLock(hg);
    if (!dst) { GlobalFree(hg); CloseClipboard(); return 0; }
    memcpy(dst, text, len);
    GlobalUnlock(hg);
    if (!SetClipboardData(CF_TEXT, hg)) { GlobalFree(hg); CloseClipboard(); return 0; }
    CloseClipboard();
    return 1;
}

void stream_host_stop(void) {
    plat_mutex_lock(&g_lock);
    if (!g_running && g_state == STREAM_STATE_OFF) { plat_mutex_unlock(&g_lock); return; }
    g_running = 0;
    g_state = STREAM_STATE_OFF;
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (g_clients[i].sock != INVALID_SOCKET) {
            closesocket(g_clients[i].sock);
            g_clients[i].sock = INVALID_SOCKET;
        }
        g_clients[i].active = 0;
        g_clients[i].buttons = 0;
    }
    HANDLE cf_proc = g_cf_proc; g_cf_proc = NULL;
    HANDLE cf_job = g_cf_job; g_cf_job = NULL;
    HANDLE cf_pipe = g_cf_pipe_r; g_cf_pipe_r = NULL;
    HANDLE srv_th = g_srv_thread; g_srv_thread = NULL;
    HANDLE cf_th = g_cf_thread; g_cf_thread = NULL;
    g_public_url[0] = 0;
    g_local_url[0] = 0;
    snprintf(g_status_msg, sizeof(g_status_msg), "Stopped");
    plat_mutex_unlock(&g_lock);

    if (cf_proc) { TerminateProcess(cf_proc, 0); WaitForSingleObject(cf_proc, 1000); CloseHandle(cf_proc); }
    if (cf_job) CloseHandle(cf_job);
    if (cf_pipe) CloseHandle(cf_pipe);
    if (srv_th) { WaitForSingleObject(srv_th, 1000); CloseHandle(srv_th); }
    if (cf_th) { WaitForSingleObject(cf_th, 1000); CloseHandle(cf_th); }
    webrtc_shutdown();

    plat_mutex_lock(&g_enc_lock);
    if (g_vpx_ok) { vpx_codec_destroy(&g_vpx); vpx_img_free(&g_vpx_img); g_vpx_ok = 0; g_vpx_w = g_vpx_h = 0; }
    if (g_hw_ok) { stream_hwenc_close(); g_hw_ok = 0; g_hw_codec = 0; g_hw_w = g_hw_h = 0; }
    free(g_nv12_buf); g_nv12_buf = NULL; g_nv12_cap = 0;
    g_active_codec = STREAM_CODEC_VP8;
    plat_mutex_unlock(&g_enc_lock);

    plat_mutex_lock(&g_aud_lock);
    if (g_opus) { opus_encoder_destroy(g_opus); g_opus = NULL; g_opus_rate = 0; g_aud_fill = 0; }
    plat_mutex_unlock(&g_aud_lock);

    rt_log("stream: server stopped");
}

int stream_host_active(void) { return g_running != 0; }

void stream_host_info(StreamHostInfo *out) {
    if (!out) return;
    int has_cf = stream_host_has_cloudflared();
    plat_mutex_lock(&g_lock);
    out->state = g_state;
    out->port = g_port;
    out->mode = g_mode;
    out->use_cf = g_use_cf;
    out->cf_installed = has_cf;
    out->cf_installing = g_cf_installing;
    int c = 0;
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) c += g_clients[i].active ? 1 : 0;
    out->clients = c;
    snprintf(out->code, sizeof(out->code), "%s", g_code);
    snprintf(out->local_url, sizeof(out->local_url), "%s", g_local_url);
    snprintf(out->public_url, sizeof(out->public_url), "%s", g_public_url);
    snprintf(out->status_msg, sizeof(out->status_msg), "%s", g_status_msg);
    snprintf(out->cf_install_msg, sizeof(out->cf_install_msg), "%s", g_cf_install_msg);
    out->frames_sent = g_frames_sent;
    out->bytes_sent = g_bytes_sent;
    plat_mutex_unlock(&g_lock);
}

static int pick_stream_codec(void) {
    int hw = stream_hwenc_probe() & ~g_hw_fail_mask;
    const char *pref = cfg_value("MTF_STREAM_CODEC");
    if (!strcmp(pref, "vp8")) return STREAM_CODEC_VP8;
    if (!strcmp(pref, "av1") && (hw & (1 << STREAM_CODEC_AV1))) return STREAM_CODEC_AV1;
    if (!strcmp(pref, "h264") && (hw & (1 << STREAM_CODEC_H264))) return STREAM_CODEC_H264;
    // Auto: prefer AV1 (GPU) -> H.264 (GPU) -> VP8 (CPU), gated by connected clients' browser decode support.
    int can_av1 = (hw & (1 << STREAM_CODEC_AV1)) != 0;
    int can_h264 = (hw & (1 << STREAM_CODEC_H264)) != 0;
    plat_mutex_lock(&g_lock);
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
        if (!g_clients[i].active) continue;
        if (!g_clients[i].caps_known) { can_av1 = 0; can_h264 = 0; break; }
        if (!g_clients[i].caps_av1) can_av1 = 0;
        if (!g_clients[i].caps_h264) can_h264 = 0;
    }
    plat_mutex_unlock(&g_lock);
    if (can_av1) return STREAM_CODEC_AV1;
    if (can_h264) return STREAM_CODEC_H264;
    return STREAM_CODEC_VP8;
}

void stream_host_codec_info(int *hw_mask, int *active_codec) {
    if (hw_mask) *hw_mask = stream_hwenc_probe() & ~g_hw_fail_mask;
    if (active_codec) *active_codec = g_active_codec;
}

static int any_client_connected(void) {
    for (int i = 0; i < STREAM_MAX_CLIENTS; i++)
        if (g_clients[i].active) return 1;
    return 0;
}

static void bgra_to_i420(const uint8_t *bgra, int stride, int w, int h, vpx_image_t *img) {
    uint8_t *Y = img->planes[VPX_PLANE_Y];
    uint8_t *U = img->planes[VPX_PLANE_U];
    uint8_t *V = img->planes[VPX_PLANE_V];
    int ys = img->stride[VPX_PLANE_Y], us = img->stride[VPX_PLANE_U], vs = img->stride[VPX_PLANE_V];
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bgra + y * stride;
        uint8_t *yr = Y + y * ys;
        for (int x = 0; x < w; x++) {
            int b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int yv = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yr[x] = (uint8_t)(yv < 0 ? 0 : yv > 255 ? 255 : yv);
        }
        if ((y & 1) == 0 && y + 1 < h) {
            const uint8_t *row2 = row + stride;
            uint8_t *ur = U + (y >> 1) * us;
            uint8_t *vr = V + (y >> 1) * vs;
            for (int x = 0; x + 1 < w; x += 2) {
                int b = (row[x * 4 + 0] + row[(x + 1) * 4 + 0] + row2[x * 4 + 0] + row2[(x + 1) * 4 + 0]) >> 2;
                int g = (row[x * 4 + 1] + row[(x + 1) * 4 + 1] + row2[x * 4 + 1] + row2[(x + 1) * 4 + 1]) >> 2;
                int r = (row[x * 4 + 2] + row[(x + 1) * 4 + 2] + row2[x * 4 + 2] + row2[(x + 1) * 4 + 2]) >> 2;
                int uv = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int vv = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                ur[x >> 1] = (uint8_t)(uv < 0 ? 0 : uv > 255 ? 255 : uv);
                vr[x >> 1] = (uint8_t)(vv < 0 ? 0 : vv > 255 ? 255 : vv);
            }
        }
    }
}

static void bgra_to_nv12(const uint8_t *bgra, int stride, int w, int h, uint8_t *nv12) {
    uint8_t *Y = nv12;
    uint8_t *UV = nv12 + (size_t)w * (size_t)h;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bgra + y * stride;
        uint8_t *yr = Y + y * w;
        for (int x = 0; x < w; x++) {
            int b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int yv = (77 * r + 150 * g + 29 * b + 128) >> 8;
            yr[x] = (uint8_t)(yv < 0 ? 0 : yv > 255 ? 255 : yv);
        }
        if ((y & 1) == 0 && y + 1 < h) {
            const uint8_t *row2 = row + stride;
            uint8_t *uvr = UV + (y >> 1) * w;
            for (int x = 0; x + 1 < w; x += 2) {
                int b = (row[x * 4 + 0] + row[(x + 1) * 4 + 0] + row2[x * 4 + 0] + row2[(x + 1) * 4 + 0]) >> 2;
                int g = (row[x * 4 + 1] + row[(x + 1) * 4 + 1] + row2[x * 4 + 1] + row2[(x + 1) * 4 + 1]) >> 2;
                int r = (row[x * 4 + 2] + row[(x + 1) * 4 + 2] + row2[x * 4 + 2] + row2[(x + 1) * 4 + 2]) >> 2;
                int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                uvr[x + 0] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
                uvr[x + 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
}

static void broadcast_video_pkt(int codec, int kf, int w, int h, uint64_t now, const uint8_t *data, size_t sz) {
    uint8_t *msg = malloc(10 + sz);
    if (!msg) return;
    uint16_t ww = (uint16_t)w, hh = (uint16_t)h;
    uint32_t ts = (uint32_t)(now / 1000000ull);
    msg[0] = 1;
    msg[1] = (uint8_t)(((codec & 0xf) << 4) | (kf ? 1 : 0));
    memcpy(msg + 2, &ww, 2);
    memcpy(msg + 4, &hh, 2);
    memcpy(msg + 6, &ts, 4);
    memcpy(msg + 10, data, sz);
    if (ws_broadcast_bin(msg, 10 + sz)) {
        plat_mutex_lock(&g_lock);
        for (int i = 0; i < STREAM_MAX_CLIENTS; i++)
            if (g_clients[i].active) g_clients[i].need_keyframe = 1;
        plat_mutex_unlock(&g_lock);
    }
    free(msg);
    g_frames_sent++;
}

void stream_host_push_video(const uint8_t *bgra, int w, int h, int stride) {
    if (!g_running || !bgra || w <= 0 || h <= 0 || !any_client_connected()) return;
    uint64_t now = plat_now_ns();
    if (now - g_last_vid_ns < 10000000ull) return;
    g_last_vid_ns = now;

    static const unsigned k_kbps[3] = {2200, 4500, 7500};
    static const unsigned k_kf[3] = {45, 45, 60};
    int q = stream_quality_mode();
    int codec = pick_stream_codec();

    plat_mutex_lock(&g_enc_lock);
    if (codec != STREAM_CODEC_VP8) {
        if (!g_hw_ok || g_hw_codec != codec || g_hw_w != w || g_hw_h != h || g_hw_q != q) {
            if (g_hw_ok) { stream_hwenc_close(); g_hw_ok = 0; }
            if (stream_hwenc_open(codec, w, h, 60, (int)k_kbps[q], (int)k_kf[q])) {
                g_hw_ok = 1; g_hw_codec = codec; g_hw_w = w; g_hw_h = h; g_hw_q = q;
            } else {
                g_hw_fail_mask |= (1 << codec);
                codec = pick_stream_codec();
            }
        }
        if (codec != STREAM_CODEC_VP8 && g_hw_ok) {
            size_t need_nv12 = (size_t)w * (size_t)h * 3 / 2;
            if (g_nv12_cap < need_nv12) {
                uint8_t *nb = realloc(g_nv12_buf, need_nv12);
                if (nb) { g_nv12_buf = nb; g_nv12_cap = need_nv12; }
            }
            if (g_nv12_buf) {
                bgra_to_nv12(bgra, stride, w, h, g_nv12_buf);
                int force_kf = 0;
                plat_mutex_lock(&g_lock);
                for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
                    if (g_clients[i].active && g_clients[i].need_keyframe) {
                        force_kf = 1;
                        g_clients[i].need_keyframe = 0;
                    }
                }
                plat_mutex_unlock(&g_lock);
                const uint8_t *pkt = NULL;
                size_t psz = 0;
                int okf = 0;
                if (stream_hwenc_encode(g_nv12_buf, w, h, force_kf, &pkt, &psz, &okf) && pkt && psz > 0) {
                    g_active_codec = codec;
                    broadcast_video_pkt(codec, okf, w, h, now, pkt, psz);
                }
                plat_mutex_unlock(&g_enc_lock);
                return;
            }
        }
    }
    g_active_codec = STREAM_CODEC_VP8;
    if (!g_vpx_ok || g_vpx_w != w || g_vpx_h != h || g_vpx_quality != q) {
        if (g_vpx_ok) { vpx_codec_destroy(&g_vpx); vpx_img_free(&g_vpx_img); g_vpx_ok = 0; }
        vpx_codec_enc_cfg_t cfg;
        if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) == VPX_CODEC_OK) {
            static const unsigned k_qmin[3] = {8, 4, 2};
            static const unsigned k_qmax[3] = {56, 48, 36};
            static const int k_cpu[3] = {-10, -8, -6};
            cfg.g_w = (unsigned)w;
            cfg.g_h = (unsigned)h;
            cfg.g_timebase.num = 1;
            cfg.g_timebase.den = 60;
            cfg.rc_target_bitrate = k_kbps[q];
            cfg.rc_min_quantizer = k_qmin[q];
            cfg.rc_max_quantizer = k_qmax[q];
            cfg.rc_end_usage = VPX_CBR;
            cfg.rc_buf_sz = 120;
            cfg.rc_buf_initial_sz = 60;
            cfg.rc_buf_optimal_sz = 60;
            cfg.g_lag_in_frames = 0;
            cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
            cfg.kf_mode = VPX_KF_AUTO;
            cfg.kf_max_dist = k_kf[q];
            cfg.g_threads = 4;
            if (vpx_codec_enc_init(&g_vpx, vpx_codec_vp8_cx(), &cfg, 0) == VPX_CODEC_OK &&
                vpx_img_alloc(&g_vpx_img, VPX_IMG_FMT_I420, (unsigned)w, (unsigned)h, 1)) {
                vpx_codec_control(&g_vpx, VP8E_SET_CPUUSED, k_cpu[q]);
                g_vpx_ok = 1;
                g_vpx_w = w;
                g_vpx_h = h;
                g_vpx_quality = q;
                g_vpx_pts = 0;
            }
        }
    }
    if (g_vpx_ok) {
        bgra_to_i420(bgra, stride, w, h, &g_vpx_img);
        int flags = 0;
        for (int i = 0; i < STREAM_MAX_CLIENTS; i++) {
            if (g_clients[i].active && g_clients[i].need_keyframe) {
                flags |= VPX_EFLAG_FORCE_KF;
                g_clients[i].need_keyframe = 0;
            }
        }
        if (vpx_codec_encode(&g_vpx, &g_vpx_img, (vpx_codec_pts_t)g_vpx_pts++, 1, flags, VPX_DL_REALTIME) == VPX_CODEC_OK) {
            vpx_codec_iter_t iter = NULL;
            const vpx_codec_cx_pkt_t *pkt;
            while ((pkt = vpx_codec_get_cx_data(&g_vpx, &iter)) != NULL) {
                if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
                int kf = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) ? 1 : 0;
                broadcast_video_pkt(STREAM_CODEC_VP8, kf, w, h, now, pkt->data.frame.buf, pkt->data.frame.sz);
            }
        }
    }
    plat_mutex_unlock(&g_enc_lock);
}

void stream_host_push_audio(const float *pcm, uint32_t frames, int channels, int rate) {
    if (!g_running || !pcm || !frames || !any_client_connected()) return;
    if (rate != 48000 || channels < 1 || channels > 8) return;
    plat_mutex_lock(&g_aud_lock);
    if (!g_opus || g_opus_rate != rate) {
        if (g_opus) opus_encoder_destroy(g_opus);
        int err = 0;
        g_opus = opus_encoder_create(rate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK) g_opus = NULL;
        g_opus_rate = rate;
        g_aud_fill = 0;
    }
    if (g_opus) {
        for (uint32_t i = 0; i < frames; i++) {
            float l = pcm[i * channels + 0];
            float r = channels >= 2 ? pcm[i * channels + 1] : l;
            int32_t il = (int32_t)(l * 32767.0f), ir = (int32_t)(r * 32767.0f);
            g_aud_buf[g_aud_fill * 2 + 0] = (int16_t)(il < -32768 ? -32768 : il > 32767 ? 32767 : il);
            g_aud_buf[g_aud_fill * 2 + 1] = (int16_t)(ir < -32768 ? -32768 : ir > 32767 ? 32767 : ir);
            g_aud_fill++;
            if (g_aud_fill == OPUS_FRAME_SAMPLES) {
                uint8_t out[1500];
                int n = opus_encode(g_opus, g_aud_buf, OPUS_FRAME_SAMPLES, out + 8, sizeof(out) - 8);
                if (n > 0) {
                    uint16_t r100 = (uint16_t)(rate / 100);
                    uint32_t ts = (uint32_t)(plat_now_ns() / 1000000ull);
                    out[0] = 2;
                    memcpy(out + 1, &r100, 2);
                    out[3] = 2;
                    memcpy(out + 4, &ts, 4);
                    ws_broadcast_bin(out, (size_t)(8 + n));
                }
                g_aud_fill = 0;
            }
        }
    }
    plat_mutex_unlock(&g_aud_lock);
}

int stream_host_remote_connected(int player) {
    if (!g_running || player < 1 || player > STREAM_MAX_CLIENTS) return 0;
    return g_clients[player - 1].active != 0;
}

int stream_host_remote_poll(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!g_running || player < 1 || player > STREAM_MAX_CLIENTS) return 0;
    int slot = player - 1;
    plat_mutex_lock(&g_lock);
    int act = g_clients[slot].active;
    if (act) {
        if (plat_now_ns() - g_clients[slot].last_input_ns < 500000000ull) {
            *buttons = g_clients[slot].buttons;
            *lx = g_clients[slot].lx;
            *ly = g_clients[slot].ly;
            *rx = g_clients[slot].rx;
            *ry = g_clients[slot].ry;
        } else {
            *buttons = 0;
            *lx = *ly = *rx = *ry = 0;
        }
    }
    plat_mutex_unlock(&g_lock);
    return act;
}
