// WebRTC DataChannel peer manager using libdatachannel (MPL-2.0) + MbedTLS.
#define RTC_STATIC 1
#include "stream_webrtc.h"

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

static constexpr int kMaxPeers = 3;

struct PeerSlot {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc_media;
    std::shared_ptr<rtc::DataChannel> dc_input;
    WebRtcSignalCb sig_cb = nullptr;
    void *sig_user = nullptr;
    WebRtcInputCb in_cb = nullptr;
    WebRtcStateCb st_cb = nullptr;
    void *cb_user = nullptr;
    bool media_open = false;
    bool active = false;
};

static std::mutex g_mtx;
static PeerSlot g_peers[kMaxPeers];
static bool g_inited = false;

static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static std::string json_get_str(const std::string &json, const char *key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string out;
    for (size_t i = pos + 1; i < json.size(); i++) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char n = json[++i];
            if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else out += n;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

void webrtc_init(void) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_inited) {
        rtc::InitLogger(rtc::LogLevel::Warning);
        g_inited = true;
    }
}

void webrtc_shutdown(void) {
    for (int i = 0; i < kMaxPeers; i++) webrtc_peer_close(i);
}

int webrtc_peer_start(int slot, const char *stun_url, const char *turn_url,
                      WebRtcSignalCb sig_cb, void *sig_user,
                      WebRtcInputCb in_cb, WebRtcStateCb st_cb, void *cb_user) {
    if (slot < 0 || slot >= kMaxPeers) return 0;
    webrtc_peer_close(slot);
    webrtc_init();

    rtc::Configuration cfg;
    if (stun_url && *stun_url) cfg.iceServers.emplace_back(stun_url);
    else cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    if (turn_url && *turn_url) cfg.iceServers.emplace_back(turn_url);

    std::lock_guard<std::mutex> lk(g_mtx);
    auto &p = g_peers[slot];
    p.sig_cb = sig_cb;
    p.sig_user = sig_user;
    p.in_cb = in_cb;
    p.st_cb = st_cb;
    p.cb_user = cb_user;
    p.media_open = false;
    p.active = true;

    try {
        p.pc = std::make_shared<rtc::PeerConnection>(cfg);
    } catch (...) {
        p.active = false;
        return 0;
    }

    p.pc->onLocalDescription([slot](rtc::Description desc) {
        WebRtcSignalCb cb = nullptr;
        void *usr = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_peers[slot].active) return;
            cb = g_peers[slot].sig_cb;
            usr = g_peers[slot].sig_user;
        }
        if (cb) {
            std::string msg = std::string("{\"type\":\"") + desc.typeString() +
                              "\",\"sdp\":\"" + json_escape(std::string(desc)) + "\"}";
            cb(usr, msg.c_str());
        }
    });

    p.pc->onLocalCandidate([slot](rtc::Candidate cand) {
        WebRtcSignalCb cb = nullptr;
        void *usr = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_peers[slot].active) return;
            cb = g_peers[slot].sig_cb;
            usr = g_peers[slot].sig_user;
        }
        if (cb) {
            std::string msg = std::string("{\"type\":\"candidate\",\"candidate\":\"") +
                              json_escape(std::string(cand)) + "\",\"mid\":\"" +
                              json_escape(cand.mid()) + "\"}";
            cb(usr, msg.c_str());
        }
    });

    p.pc->onStateChange([slot](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            WebRtcStateCb cb = nullptr;
            void *usr = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_peers[slot].media_open = false;
                cb = g_peers[slot].st_cb;
                usr = g_peers[slot].cb_user;
            }
            if (cb) cb(usr, slot, 0);
        }
    });

    rtc::DataChannelInit init_media;
    init_media.reliability.unordered = false;
    init_media.reliability.maxPacketLifeTime = std::chrono::milliseconds(150);

    rtc::DataChannelInit init_unord;
    init_unord.reliability.unordered = true;
    init_unord.reliability.maxRetransmits = 0;

    p.dc_media = p.pc->createDataChannel("media", init_media);
    p.dc_media->onOpen([slot]() {
        WebRtcStateCb cb = nullptr;
        void *usr = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_peers[slot].active) return;
            g_peers[slot].media_open = true;
            cb = g_peers[slot].st_cb;
            usr = g_peers[slot].cb_user;
        }
        if (cb) cb(usr, slot, 1);
    });
    p.dc_media->onClosed([slot]() {
        WebRtcStateCb cb = nullptr;
        void *usr = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_peers[slot].media_open = false;
            cb = g_peers[slot].st_cb;
            usr = g_peers[slot].cb_user;
        }
        if (cb) cb(usr, slot, 0);
    });

    p.dc_input = p.pc->createDataChannel("input", init_unord);
    p.dc_input->onMessage([slot](rtc::message_variant msg) {
        if (!std::holds_alternative<rtc::binary>(msg)) return;
        const auto &bin = std::get<rtc::binary>(msg);
        WebRtcInputCb cb = nullptr;
        void *usr = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_peers[slot].active) return;
            cb = g_peers[slot].in_cb;
            usr = g_peers[slot].cb_user;
        }
        if (cb && !bin.empty())
            cb(usr, slot, reinterpret_cast<const uint8_t *>(bin.data()), bin.size());
    });

    return 1;
}

void webrtc_peer_handle_signal(int slot, const char *json_msg) {
    if (slot < 0 || slot >= kMaxPeers || !json_msg) return;
    std::string s(json_msg);
    std::string type = json_get_str(s, "type");
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_peers[slot].active) return;
        pc = g_peers[slot].pc;
    }
    if (!pc) return;
    try {
        if (type == "answer") {
            std::string sdp = json_get_str(s, "sdp");
            if (!sdp.empty()) pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
        } else if (type == "candidate") {
            std::string cand = json_get_str(s, "candidate");
            std::string mid = json_get_str(s, "mid");
            if (!cand.empty()) pc->addRemoteCandidate(rtc::Candidate(cand, mid));
        }
    } catch (...) {
    }
}

void webrtc_peer_close(int slot) {
    if (slot < 0 || slot >= kMaxPeers) return;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dm, di;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_peers[slot].active = false;
        g_peers[slot].media_open = false;
        pc = std::move(g_peers[slot].pc);
        dm = std::move(g_peers[slot].dc_media);
        di = std::move(g_peers[slot].dc_input);
        g_peers[slot].sig_cb = nullptr;
        g_peers[slot].in_cb = nullptr;
        g_peers[slot].st_cb = nullptr;
    }
    try { if (dm) dm->close(); } catch (...) {}
    try { if (di) di->close(); } catch (...) {}
    try { if (pc) pc->close(); } catch (...) {}
}

int webrtc_peer_is_open(int slot) {
    if (slot < 0 || slot >= kMaxPeers) return 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_peers[slot].active && g_peers[slot].media_open ? 1 : 0;
}

int webrtc_broadcast_media(const uint8_t *data, size_t len) {
    if (!data || !len) return 0;
    const bool is_video_delta = (len >= 2 && data[0] == 1 && (data[1] & 1) == 0);
    std::shared_ptr<rtc::DataChannel> dcs[kMaxPeers];
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (int i = 0; i < kMaxPeers; i++) {
            if (g_peers[i].active && g_peers[i].media_open && g_peers[i].dc_media)
                dcs[i] = g_peers[i].dc_media;
        }
    }
    int dropped = 0;
    for (int i = 0; i < kMaxPeers; i++) {
        if (!dcs[i]) continue;
        try {
            if (dcs[i]->isOpen()) {
                if (is_video_delta && dcs[i]->bufferedAmount() > 384 * 1024) {
                    dropped = 1;
                    continue;
                }
                dcs[i]->send(reinterpret_cast<const rtc::byte *>(data), len);
            }
        } catch (...) {
        }
    }
    return dropped;
}
