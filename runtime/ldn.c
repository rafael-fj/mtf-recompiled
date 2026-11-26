// Clean-room Horizon OS nn::ldn (Link Play) & nn::socket UDP HLE over Winsock2 / POSIX sockets.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef int socklen_t;
static int sock_errno(void) { return WSAGetLastError(); }
static void sock_set_nonblock(SOCKET s, int nb) { u_long v = nb ? 1 : 0; ioctlsocket(s, FIONBIO, &v); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAPOLLFD struct pollfd
#define WSAPoll poll
static int sock_errno(void) { return errno; }
static void sock_set_nonblock(SOCKET s, int nb) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "ldn.h"
#include "platform/platform.h"

#define LDN_MAGIC 0x314E444Cu /* 'LDN1' */
#define LDN_DEFAULT_PORT 11452
#define LDN_SUBNET_BASE 0x0A720000u /* 10.114.0.0 */
#define LDN_SUBNET_MASK 0xFFFF0000u /* 255.255.0.0 */
#define LDN_MAX_NODES 8
#define LDN_MAX_SOCKS 64
#define LDN_MAX_PORTS 32
#define LDN_SOCK_FD_BASE 0x2000

enum {
    PKT_SCAN_REQ = 1,
    PKT_SCAN_RSP = 2,
    PKT_JOIN_REQ = 3,
    PKT_JOIN_RSP = 4,
    PKT_SYNC = 5,
    PKT_LEAVE = 6,
    PKT_PORT_MAP = 7,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t type;
    int8_t node_id;
    uint16_t port_a;
    uint16_t port_b;
    uint16_t payload_len;
} PktHdr;

typedef struct {
    uint32_t real_ip;    // network byte order
    uint16_t ctrl_port;  // host byte order
    uint8_t active;
    uint8_t reserved;
} PeerEndpoint;

typedef struct {
    uint16_t game_port;  // host byte order
    uint16_t real_port;  // host byte order
} PortMapping;

typedef struct {
    LdnNetworkInfo net;
    PeerEndpoint peers[LDN_MAX_NODES];
    PortMapping ports[LDN_MAX_NODES][LDN_MAX_PORTS];
} SyncPayload;
#pragma pack(pop)

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static int g_wsa_ok;
static int g_ctrl_started;
static SOCKET g_ctrl_sock = INVALID_SOCKET;
static uint16_t g_ctrl_port;

static uint32_t g_state = LDN_STATE_NONE;
static uint64_t g_state_event;
static int8_t g_my_node = -1;
static uint32_t g_host_ip;    // network byte order
static uint16_t g_host_port;  // host byte order
static LdnNetworkInfo g_net;
static PeerEndpoint g_peers[LDN_MAX_NODES];
static PortMapping g_ports[LDN_MAX_NODES][LDN_MAX_PORTS];

static struct {
    LdnNetworkInfo net;
    uint32_t host_ip;
    uint16_t host_port;
    uint64_t seen_ns;
} g_scan_cache[16];
static int g_scan_n;

typedef struct {
    int used;
    SOCKET s;
    uint16_t game_port;
    uint16_t real_port;
} LdnSock;

static LdnSock g_socks[LDN_MAX_SOCKS];
static _Thread_local int g_last_errno;

int ldn_enabled(void) {
    const char *v = cfg_value("MTF_NET");
    return !v || !*v || strcmp(v, "offline") != 0;
}

static uint16_t base_ctrl_port(void) {
    int p = cfg_int("MTF_LDN_PORT");
    return (p > 0 && p < 65535) ? (uint16_t)p : LDN_DEFAULT_PORT;
}

static void signal_state_event(void) {
    if (g_state_event) kern_signal_system_event(g_state_event);
}

static void wsa_init_once(void) {
    if (g_wsa_ok) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) g_wsa_ok = 1;
#else
    g_wsa_ok = 1;
#endif
}

static void send_ctrl_to(uint32_t ip_net, uint16_t port_host, uint8_t type, int8_t node_id,
                         uint16_t port_a, uint16_t port_b, const void *payload, uint16_t plen) {
    if (g_ctrl_sock == INVALID_SOCKET) return;
    uint8_t buf[sizeof(PktHdr) + sizeof(SyncPayload)];
    if (plen > sizeof(SyncPayload)) return;
    PktHdr h = {LDN_MAGIC, type, node_id, port_a, port_b, plen};
    memcpy(buf, &h, sizeof h);
    if (plen && payload) memcpy(buf + sizeof h, payload, plen);
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip_net;
    dst.sin_port = htons(port_host);
    sendto(g_ctrl_sock, (const char *)buf, (int)(sizeof h + plen), 0, (const struct sockaddr *)&dst, sizeof dst);
}

static void broadcast_sync_locked(void) {
    SyncPayload sp;
    memcpy(&sp.net, &g_net, sizeof sp.net);
    memcpy(sp.peers, g_peers, sizeof sp.peers);
    memcpy(sp.ports, g_ports, sizeof sp.ports);
    for (int i = 1; i < LDN_MAX_NODES; i++) {
        if (g_net.ldn.nodes[i].is_connected && g_peers[i].active)
            send_ctrl_to(g_peers[i].real_ip, g_peers[i].ctrl_port, PKT_SYNC, (int8_t)i, 0, 0, &sp, sizeof sp);
    }
}

static void announce_my_ports_locked(void) {
    if (g_my_node < 0 || g_my_node >= LDN_MAX_NODES) return;
    for (int s = 0; s < LDN_MAX_SOCKS; s++) {
        if (!g_socks[s].used || !g_socks[s].game_port || !g_socks[s].real_port) continue;
        if (g_state == LDN_STATE_ACCESS_POINT_CREATED) {
            broadcast_sync_locked();
            break;
        } else if (g_state == LDN_STATE_STATION_CONNECTED && g_host_ip) {
            send_ctrl_to(g_host_ip, g_host_port, PKT_PORT_MAP, g_my_node,
                         g_socks[s].game_port, g_socks[s].real_port, NULL, 0);
        }
    }
}

static void handle_ctrl_packet(const uint8_t *buf, int len, const struct sockaddr_in *from) {
    if (len < (int)sizeof(PktHdr)) return;
    PktHdr h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != LDN_MAGIC || len < (int)(sizeof h + h.payload_len)) return;
    const uint8_t *pl = buf + sizeof h;

    plat_mutex_lock(&g_lock);
    switch (h.type) {
    case PKT_SCAN_REQ:
        if (g_state == LDN_STATE_ACCESS_POINT_CREATED) {
            send_ctrl_to(from->sin_addr.s_addr, ntohs(from->sin_port), PKT_SCAN_RSP, 0,
                         g_ctrl_port, 0, &g_net, sizeof g_net);
        }
        break;
    case PKT_SCAN_RSP:
        if (h.payload_len == sizeof(LdnNetworkInfo)) {
            LdnNetworkInfo ni;
            memcpy(&ni, pl, sizeof ni);
            int idx = -1;
            for (int i = 0; i < g_scan_n; i++) {
                if (memcmp(&g_scan_cache[i].net.network_id, &ni.network_id, sizeof ni.network_id) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0 && g_scan_n < 16) idx = g_scan_n++;
            if (idx >= 0) {
                g_scan_cache[idx].net = ni;
                g_scan_cache[idx].host_ip = from->sin_addr.s_addr;
                g_scan_cache[idx].host_port = h.port_a ? h.port_a : ntohs(from->sin_port);
                g_scan_cache[idx].seen_ns = plat_now_ns();
            }
        }
        break;
    case PKT_JOIN_REQ:
        if (g_state == LDN_STATE_ACCESS_POINT_CREATED && h.payload_len == sizeof(LdnUserConfig)) {
            const LdnUserConfig *uc = (const LdnUserConfig *)pl;
            int slot = -1;
            for (int i = 1; i < g_net.ldn.node_count_max && i < LDN_MAX_NODES; i++) {
                if (g_peers[i].active && g_peers[i].real_ip == from->sin_addr.s_addr &&
                    g_peers[i].ctrl_port == ntohs(from->sin_port)) {
                    slot = i;
                    break;
                }
                if (slot < 0 && !g_net.ldn.nodes[i].is_connected) slot = i;
            }
            if (slot > 0) {
                g_peers[slot] = (PeerEndpoint){from->sin_addr.s_addr, ntohs(from->sin_port), 1, 0};
                LdnNodeInfo *nd = &g_net.ldn.nodes[slot];
                memset(nd, 0, sizeof *nd);
                nd->ipv4_addr = LDN_SUBNET_BASE | (uint32_t)(slot + 1);
                nd->mac[0] = 0x02;
                nd->mac[5] = (uint8_t)(slot + 1);
                nd->node_id = (int8_t)slot;
                nd->is_connected = 1;
                memcpy(nd->user_name, uc->user_name, sizeof nd->user_name);
                uint8_t cnt = 0;
                for (int i = 0; i < LDN_MAX_NODES; i++) cnt += g_net.ldn.nodes[i].is_connected != 0;
                g_net.ldn.node_count = cnt;
                SyncPayload sp;
                memcpy(&sp.net, &g_net, sizeof sp.net);
                memcpy(sp.peers, g_peers, sizeof sp.peers);
                memcpy(sp.ports, g_ports, sizeof sp.ports);
                send_ctrl_to(from->sin_addr.s_addr, ntohs(from->sin_port), PKT_JOIN_RSP, (int8_t)slot, 0, 0, &sp, sizeof sp);
                broadcast_sync_locked();
                signal_state_event();
                rt_log("ldn: node %d (%s) joined from %u.%u.%u.%u:%u", slot, nd->user_name,
                       (unsigned)(from->sin_addr.s_addr & 0xff), (unsigned)(from->sin_addr.s_addr >> 8 & 0xff),
                       (unsigned)(from->sin_addr.s_addr >> 16 & 0xff), (unsigned)(from->sin_addr.s_addr >> 24 & 0xff),
                       ntohs(from->sin_port));
            }
        }
        break;
    case PKT_JOIN_RSP:
    case PKT_SYNC:
        if (h.payload_len == sizeof(SyncPayload)) {
            const SyncPayload *sp = (const SyncPayload *)pl;
            memcpy(&g_net, &sp->net, sizeof g_net);
            memcpy(g_peers, sp->peers, sizeof g_peers);
            memcpy(g_ports, sp->ports, sizeof g_ports);
            if (h.type == PKT_JOIN_RSP) {
                g_my_node = h.node_id;
                g_state = LDN_STATE_STATION_CONNECTED;
            }
            signal_state_event();
        }
        break;
    case PKT_PORT_MAP:
        if (g_state == LDN_STATE_ACCESS_POINT_CREATED && h.node_id > 0 && h.node_id < LDN_MAX_NODES) {
            int n = h.node_id, placed = 0;
            for (int i = 0; i < LDN_MAX_PORTS; i++) {
                if (g_ports[n][i].game_port == h.port_a) {
                    g_ports[n][i].real_port = h.port_b;
                    placed = 1;
                    break;
                }
            }
            if (!placed) {
                for (int i = 0; i < LDN_MAX_PORTS; i++) {
                    if (!g_ports[n][i].game_port) {
                        g_ports[n][i] = (PortMapping){h.port_a, h.port_b};
                        break;
                    }
                }
            }
            broadcast_sync_locked();
        }
        break;
    case PKT_LEAVE:
        if (g_state == LDN_STATE_ACCESS_POINT_CREATED && h.node_id > 0 && h.node_id < LDN_MAX_NODES) {
            memset(&g_net.ldn.nodes[h.node_id], 0, sizeof g_net.ldn.nodes[h.node_id]);
            memset(&g_peers[h.node_id], 0, sizeof g_peers[h.node_id]);
            memset(g_ports[h.node_id], 0, sizeof g_ports[h.node_id]);
            uint8_t cnt = 0;
            for (int i = 0; i < LDN_MAX_NODES; i++) cnt += g_net.ldn.nodes[i].is_connected != 0;
            g_net.ldn.node_count = cnt;
            broadcast_sync_locked();
            signal_state_event();
        }
        break;
    }
    plat_mutex_unlock(&g_lock);
}

static void ldn_ctrl_thread(void *arg) {
    (void)arg;
    uint8_t buf[sizeof(PktHdr) + sizeof(SyncPayload) + 64];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        int n = recvfrom(g_ctrl_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) handle_ctrl_packet(buf, n, &from);
        else plat_wait_ns(5000000);
    }
}

static void ensure_ctrl_started(int prefer_host_port) {
    wsa_init_once();
    if (!g_wsa_ok) return;
    if (g_ctrl_sock != INVALID_SOCKET) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    BOOL bc = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&bc, sizeof bc);
    sock_set_nonblock(s, 1);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    uint16_t p = prefer_host_port ? base_ctrl_port() : 0;
    sa.sin_port = htons(p);
    if (bind(s, (const struct sockaddr *)&sa, sizeof sa) != 0) {
        sa.sin_port = 0;
        if (bind(s, (const struct sockaddr *)&sa, sizeof sa) != 0) {
            closesocket(s);
            return;
        }
    }
    socklen_t slen = sizeof sa;
    getsockname(s, (struct sockaddr *)&sa, &slen);
    g_ctrl_sock = s;
    g_ctrl_port = ntohs(sa.sin_port);
    if (!g_ctrl_started) {
        g_ctrl_started = 1;
        plat_thread_spawn(ldn_ctrl_thread, NULL, "ldn_ctrl", 0);
    }
    rt_log("ldn: control UDP socket bound on port %u", g_ctrl_port);
}

uint32_t ldn_initialize(void) {
    if (!ldn_enabled()) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    wsa_init_once();
    g_state = LDN_STATE_INITIALIZED;
    g_my_node = -1;
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    rt_log("ldn: initialized (Link Play HLE active)");
    return LDN_RES_OK;
}

void ldn_finalize(void) {
    plat_mutex_lock(&g_lock);
    g_state = LDN_STATE_NONE;
    g_my_node = -1;
    plat_mutex_unlock(&g_lock);
}

uint32_t ldn_get_state(void) { return __atomic_load_n(&g_state, __ATOMIC_RELAXED); }
void ldn_attach_state_event(uint64_t event_addr) { g_state_event = event_addr; }

uint32_t ldn_open_access_point(void) {
    if (!ldn_enabled()) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    ensure_ctrl_started(1);
    g_state = LDN_STATE_ACCESS_POINT_OPENED;
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_close_access_point(void) {
    plat_mutex_lock(&g_lock);
    g_state = LDN_STATE_INITIALIZED;
    g_my_node = -1;
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_open_station(void) {
    if (!ldn_enabled()) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    ensure_ctrl_started(0);
    g_state = LDN_STATE_STATION_OPENED;
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_close_station(void) {
    plat_mutex_lock(&g_lock);
    g_state = LDN_STATE_INITIALIZED;
    g_my_node = -1;
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_create_network(const LdnNetworkConfig *nc, const LdnSecurityConfig *sc, const LdnUserConfig *uc) {
    if (!ldn_enabled()) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    ensure_ctrl_started(1);
    memset(&g_net, 0, sizeof g_net);
    memset(g_peers, 0, sizeof g_peers);
    memset(g_ports, 0, sizeof g_ports);
    if (nc) {
        memcpy(g_net.network_id.raw, nc->intent_id, 16);
        uint64_t sid = plat_now_ns();
        memcpy(g_net.network_id.session_id, &sid, sizeof sid);
        g_net.common.channel = nc->channel ? nc->channel : 6;
        g_net.ldn.node_count_max = nc->node_count_max ? nc->node_count_max : 8;
        g_net.ldn.nodes[0].local_comm_version = (int16_t)nc->local_comm_version;
    } else {
        g_net.ldn.node_count_max = 8;
    }
    if (sc) g_net.ldn.security_mode = sc->security_mode;
    g_net.common.bssid[0] = 0x02;
    g_net.common.bssid[5] = 0x01;
    g_net.common.link_level = 3;
    g_net.common.network_type = 2; // LDN
    g_net.ldn.node_count = 1;
    g_net.ldn.nodes[0].ipv4_addr = LDN_SUBNET_BASE | 1u;
    g_net.ldn.nodes[0].mac[0] = 0x02;
    g_net.ldn.nodes[0].mac[5] = 0x01;
    g_net.ldn.nodes[0].node_id = 0;
    g_net.ldn.nodes[0].is_connected = 1;
    if (uc) memcpy(g_net.ldn.nodes[0].user_name, uc->user_name, sizeof g_net.ldn.nodes[0].user_name);
    g_peers[0] = (PeerEndpoint){htonl(INADDR_LOOPBACK), g_ctrl_port, 1, 0};
    g_my_node = 0;
    g_state = LDN_STATE_ACCESS_POINT_CREATED;
    announce_my_ports_locked();
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    rt_log("ldn: network created on port %u (max %u nodes, user '%s')", g_ctrl_port, g_net.ldn.node_count_max,
           g_net.ldn.nodes[0].user_name);
    return LDN_RES_OK;
}

uint32_t ldn_destroy_network(void) {
    plat_mutex_lock(&g_lock);
    g_state = LDN_STATE_ACCESS_POINT_OPENED;
    g_my_node = -1;
    memset(&g_net, 0, sizeof g_net);
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_set_advertise_data(const void *data, uint64_t size) {
    if (!ldn_enabled()) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    if (size > sizeof g_net.ldn.advertise_data) size = sizeof g_net.ldn.advertise_data;
    g_net.ldn.advertise_data_size = (uint16_t)size;
    if (size && data) memcpy(g_net.ldn.advertise_data, data, (size_t)size);
    broadcast_sync_locked();
    plat_mutex_unlock(&g_lock);
    return LDN_RES_OK;
}

uint32_t ldn_scan(LdnNetworkInfo *out, int32_t *count, int32_t max_count, const LdnScanFilter *filter) {
    (void)filter;
    if (!ldn_enabled()) {
        if (count) *count = 0;
        return LDN_RES_UNAVAILABLE;
    }
    plat_mutex_lock(&g_lock);
    ensure_ctrl_started(0);
    g_scan_n = 0;
    uint16_t p = base_ctrl_port();
    const char *host = cfg_get("MTF_LDN_HOST");
    if (host && *host) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
            uint32_t ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            send_ctrl_to(ip, p, PKT_SCAN_REQ, -1, g_ctrl_port, 0, NULL, 0);
            freeaddrinfo(res);
        }
    }
    send_ctrl_to(htonl(INADDR_BROADCAST), p, PKT_SCAN_REQ, -1, g_ctrl_port, 0, NULL, 0);
    send_ctrl_to(htonl(INADDR_LOOPBACK), p, PKT_SCAN_REQ, -1, g_ctrl_port, 0, NULL, 0);
    plat_mutex_unlock(&g_lock);

    int away = kern_core_away();
    plat_wait_ns(200000000ull); // 200ms scan window
    kern_core_back(&away);

    plat_mutex_lock(&g_lock);
    int32_t n = 0;
    for (int i = 0; i < g_scan_n && n < max_count; i++) {
        if (out) out[n] = g_scan_cache[i].net;
        n++;
    }
    if (count) *count = n;
    plat_mutex_unlock(&g_lock);
    return LDN_RES_OK;
}

uint32_t ldn_connect(const LdnNetworkInfo *info, const LdnSecurityConfig *sc, const LdnUserConfig *uc) {
    (void)sc;
    if (!ldn_enabled() || !info) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    ensure_ctrl_started(0);
    uint32_t hip = htonl(INADDR_LOOPBACK);
    uint16_t hport = base_ctrl_port();
    for (int i = 0; i < g_scan_n; i++) {
        if (memcmp(&g_scan_cache[i].net.network_id, &info->network_id, sizeof info->network_id) == 0) {
            hip = g_scan_cache[i].host_ip;
            hport = g_scan_cache[i].host_port;
            break;
        }
    }
    g_host_ip = hip;
    g_host_port = hport;
    LdnUserConfig u = {0};
    if (uc) u = *uc;
    send_ctrl_to(hip, hport, PKT_JOIN_REQ, -1, g_ctrl_port, 0, &u, sizeof u);
    plat_mutex_unlock(&g_lock);

    for (int t = 0; t < 50; t++) {
        if (ldn_get_state() == LDN_STATE_STATION_CONNECTED) {
            plat_mutex_lock(&g_lock);
            announce_my_ports_locked();
            plat_mutex_unlock(&g_lock);
            return LDN_RES_OK;
        }
        int away = kern_core_away();
        plat_wait_ns(20000000ull);
        kern_core_back(&away);
    }
    return LDN_RES_CONNECTION_FAILED;
}

uint32_t ldn_disconnect(void) {
    plat_mutex_lock(&g_lock);
    if (g_my_node > 0 && g_host_ip) send_ctrl_to(g_host_ip, g_host_port, PKT_LEAVE, g_my_node, 0, 0, NULL, 0);
    g_state = LDN_STATE_STATION_OPENED;
    g_my_node = -1;
    memset(&g_net, 0, sizeof g_net);
    plat_mutex_unlock(&g_lock);
    signal_state_event();
    return LDN_RES_OK;
}

uint32_t ldn_get_network_info(LdnNetworkInfo *out) {
    if (!out) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    if (g_state != LDN_STATE_ACCESS_POINT_CREATED && g_state != LDN_STATE_STATION_CONNECTED) {
        plat_mutex_unlock(&g_lock);
        return LDN_RES_INVALID_STATE;
    }
    *out = g_net;
    plat_mutex_unlock(&g_lock);
    return LDN_RES_OK;
}

uint32_t ldn_get_ipv4_address(uint32_t *addr_out, uint32_t *mask_out) {
    plat_mutex_lock(&g_lock);
    int ok = g_my_node >= 0 && (g_state == LDN_STATE_ACCESS_POINT_CREATED || g_state == LDN_STATE_STATION_CONNECTED);
    uint32_t ip = ok ? (LDN_SUBNET_BASE | (uint32_t)(g_my_node + 1)) : 0;
    plat_mutex_unlock(&g_lock);
    if (addr_out) *addr_out = ip;
    if (mask_out) *mask_out = ok ? LDN_SUBNET_MASK : 0;
    return ok ? LDN_RES_OK : LDN_RES_INVALID_STATE;
}

uint32_t ldn_get_security_parameter(uint8_t out[32]) {
    if (!out) return LDN_RES_UNAVAILABLE;
    plat_mutex_lock(&g_lock);
    memcpy(out, g_net.ldn.security_param, 32);
    plat_mutex_unlock(&g_lock);
    return LDN_RES_OK;
}

int ldn_nifm_available(void) { return ldn_enabled(); }

uint32_t ldn_nifm_ip_info(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns1, uint32_t *dns2) {
    uint32_t a = 0, m = 0;
    if (ldn_get_ipv4_address(&a, &m) != LDN_RES_OK) {
        a = LDN_SUBNET_BASE | 1u;
        m = LDN_SUBNET_MASK;
    }
    uint32_t net_ip = htonl(a), net_mask = htonl(m), net_gw = htonl(LDN_SUBNET_BASE | 1u);
    if (ip) *ip = net_ip;
    if (mask) *mask = net_mask;
    if (gw) *gw = net_gw;
    if (dns1) *dns1 = net_gw;
    if (dns2) *dns2 = 0;
    return 0;
}

// ---- nn::socket UDP backend
static int map_wsa_errno(int w) {
#ifdef _WIN32
    switch (w) {
    case WSAEWOULDBLOCK: return 35;  // EAGAIN / EWOULDBLOCK on Horizon BSD
    case WSAEADDRINUSE: return 48;
    case WSAENETDOWN: return 50;
    case WSAECONNRESET: return 54;
    case WSAENOTCONN: return 57;
    default: return 22; // EINVAL
    }
#else
    if (w == EAGAIN || w == EWOULDBLOCK) return 35;
    if (w == EADDRINUSE) return 48;
    if (w == ENETDOWN) return 50;
    if (w == ECONNRESET) return 54;
    if (w == ENOTCONN) return 57;
    return 22;
#endif
}

static LdnSock *get_sock(int fd) {
    int i = fd - LDN_SOCK_FD_BASE;
    return i >= 0 && i < LDN_MAX_SOCKS && g_socks[i].used ? &g_socks[i] : NULL;
}

int ldn_sock_is_fd(int fd) { return get_sock(fd) != NULL; }
int ldn_sock_last_errno(void) { return g_last_errno; }

int ldn_sock_socket(int family, int type, int proto) {
    if (!ldn_enabled()) { g_last_errno = 50; return -1; }
    wsa_init_once();
    if (!g_wsa_ok) { g_last_errno = 50; return -1; }
    int wf = (family == 2 || family == 0) ? AF_INET : family;
    int wt = (type == 2) ? SOCK_DGRAM : (type == 1) ? SOCK_STREAM : type;
    SOCKET s = socket(wf, wt, proto);
    if (s == INVALID_SOCKET) { g_last_errno = map_wsa_errno(sock_errno()); return -1; }
    BOOL bc = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&bc, sizeof bc);
    plat_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < LDN_MAX_SOCKS; i++) if (!g_socks[i].used) { idx = i; break; }
    if (idx >= 0) g_socks[idx] = (LdnSock){1, s, 0, 0};
    plat_mutex_unlock(&g_lock);
    if (idx < 0) { closesocket(s); g_last_errno = 24; return -1; }
    return LDN_SOCK_FD_BASE + idx;
}

int ldn_sock_bind(int fd, const void *addr, uint32_t addrlen) {
    (void)addrlen;
    LdnSock *sk = get_sock(fd);
    if (!sk || !addr) { g_last_errno = 9; return -1; }
    const uint8_t *b = addr;
    uint16_t game_port = ntohs(*(const uint16_t *)(b + 2));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(game_port);
    if (bind(sk->s, (const struct sockaddr *)&sa, sizeof sa) != 0) {
        sa.sin_port = 0;
        if (bind(sk->s, (const struct sockaddr *)&sa, sizeof sa) != 0) {
            g_last_errno = map_wsa_errno(sock_errno());
            return -1;
        }
    }
    socklen_t slen = sizeof sa;
    getsockname(sk->s, (struct sockaddr *)&sa, &slen);
    uint16_t real_port = ntohs(sa.sin_port);
    plat_mutex_lock(&g_lock);
    sk->game_port = game_port ? game_port : real_port;
    sk->real_port = real_port;
    if (g_my_node >= 0 && g_my_node < LDN_MAX_NODES) {
        for (int i = 0; i < LDN_MAX_PORTS; i++) {
            if (!g_ports[g_my_node][i].game_port || g_ports[g_my_node][i].game_port == sk->game_port) {
                g_ports[g_my_node][i] = (PortMapping){sk->game_port, sk->real_port};
                break;
            }
        }
        announce_my_ports_locked();
    }
    plat_mutex_unlock(&g_lock);
    return 0;
}

int ldn_sock_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    LdnSock *sk = get_sock(fd);
    if (!sk) { g_last_errno = 9; return -1; }
    // Horizon SOL_SOCKET = 0xffff; SO_NONBLOCK = 0x1022, SO_BROADCAST = 0x0020, SO_REUSEADDR = 0x0004
    if (level == 0xffff && optname == 0x1022 && optval && optlen >= 4) {
        sock_set_nonblock(sk->s, *(const uint32_t *)optval ? 1 : 0);
        return 0;
    }
    if (level == 0xffff && optname == 0x0020 && optval && optlen >= 4) {
        BOOL bc = *(const uint32_t *)optval ? TRUE : FALSE;
        setsockopt(sk->s, SOL_SOCKET, SO_BROADCAST, (const char *)&bc, sizeof bc);
        return 0;
    }
    if (level == 0xffff && optname == 0x0004 && optval && optlen >= 4) {
        BOOL ra = *(const uint32_t *)optval ? TRUE : FALSE;
        setsockopt(sk->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&ra, sizeof ra);
        return 0;
    }
    return 0;
}

static uint16_t lookup_real_port_locked(int node, uint16_t game_port) {
    for (int i = 0; i < LDN_MAX_PORTS; i++)
        if (g_ports[node][i].game_port == game_port && g_ports[node][i].real_port) return g_ports[node][i].real_port;
    return game_port;
}

int64_t ldn_sock_sendto(int fd, const void *buf, uint64_t len, int flags, const void *addr, uint32_t addrlen) {
    (void)flags;
    (void)addrlen;
    LdnSock *sk = get_sock(fd);
    if (!sk || !addr) { g_last_errno = 9; return -1; }
    const uint8_t *b = addr;
    uint16_t dst_port = ntohs(*(const uint16_t *)(b + 2));
    uint32_t dst_ip_host = ntohl(*(const uint32_t *)(b + 4));

    if ((dst_ip_host & LDN_SUBNET_MASK) == LDN_SUBNET_BASE) {
        uint32_t host_part = dst_ip_host & 0xFFFFu;
        if (host_part == 0xFFFFu) {
            // Virtual LDN broadcast -> send to every connected peer
            plat_mutex_lock(&g_lock);
            for (int n = 0; n < LDN_MAX_NODES; n++) {
                if (n == g_my_node || !g_net.ldn.nodes[n].is_connected || !g_peers[n].active) continue;
                struct sockaddr_in sa = {0};
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = g_peers[n].real_ip;
                sa.sin_port = htons(lookup_real_port_locked(n, dst_port));
                sendto(sk->s, (const char *)buf, (int)len, 0, (const struct sockaddr *)&sa, sizeof sa);
            }
            plat_mutex_unlock(&g_lock);
            return (int64_t)len;
        }
        int node = (int)host_part - 1;
        if (node >= 0 && node < LDN_MAX_NODES) {
            plat_mutex_lock(&g_lock);
            int active = g_peers[node].active;
            uint32_t rip = g_peers[node].real_ip;
            uint16_t rport = lookup_real_port_locked(node, dst_port);
            plat_mutex_unlock(&g_lock);
            if (active) {
                struct sockaddr_in sa = {0};
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = rip;
                sa.sin_port = htons(rport);
                int sent = sendto(sk->s, (const char *)buf, (int)len, 0, (const struct sockaddr *)&sa, sizeof sa);
                if (sent < 0) { g_last_errno = map_wsa_errno(sock_errno()); return -1; }
                return sent;
            }
        }
    }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(dst_ip_host);
    sa.sin_port = htons(dst_port);
    int sent = sendto(sk->s, (const char *)buf, (int)len, 0, (const struct sockaddr *)&sa, sizeof sa);
    if (sent < 0) { g_last_errno = map_wsa_errno(sock_errno()); return -1; }
    return sent;
}

int64_t ldn_sock_recvfrom(int fd, void *buf, uint64_t len, int flags, void *addr, uint32_t *addrlen) {
    (void)flags;
    LdnSock *sk = get_sock(fd);
    if (!sk) { g_last_errno = 9; return -1; }
    struct sockaddr_in sa = {0};
    socklen_t slen = sizeof sa;
    int got = recvfrom(sk->s, (char *)buf, (int)len, 0, (struct sockaddr *)&sa, &slen);
    if (got < 0) { g_last_errno = map_wsa_errno(sock_errno()); return -1; }
    if (addr && addrlen && *addrlen >= 16) {
        uint32_t src_ip = sa.sin_addr.s_addr;
        uint16_t src_port = ntohs(sa.sin_port);
        uint32_t virt_ip_net = src_ip;
        uint16_t virt_port = src_port;
        plat_mutex_lock(&g_lock);
        for (int n = 0; n < LDN_MAX_NODES; n++) {
            if (!g_peers[n].active || g_peers[n].real_ip != src_ip) continue;
            for (int p = 0; p < LDN_MAX_PORTS; p++) {
                if (g_ports[n][p].real_port == src_port) {
                    virt_ip_net = htonl(LDN_SUBNET_BASE | (uint32_t)(n + 1));
                    virt_port = g_ports[n][p].game_port;
                    break;
                }
            }
            if (virt_ip_net != src_ip) break;
        }
        plat_mutex_unlock(&g_lock);
        uint8_t *out = addr;
        memset(out, 0, 16);
        out[0] = 16;
        out[1] = 2; // AF_INET
        *(uint16_t *)(out + 2) = htons(virt_port);
        *(uint32_t *)(out + 4) = virt_ip_net;
        *addrlen = 16;
    }
    return got;
}

int ldn_sock_close(int fd) {
    LdnSock *sk = get_sock(fd);
    if (!sk) return -1;
    closesocket(sk->s);
    plat_mutex_lock(&g_lock);
    sk->used = 0;
    sk->s = INVALID_SOCKET;
    plat_mutex_unlock(&g_lock);
    return 0;
}

int ldn_sock_shutdown(int fd, int how) {
    LdnSock *sk = get_sock(fd);
    if (!sk) { g_last_errno = 9; return -1; }
    return shutdown(sk->s, how) == 0 ? 0 : -1;
}

int ldn_sock_poll_ready(int fd, int16_t events, int16_t *revents) {
    LdnSock *sk = get_sock(fd);
    if (!sk) return 0;
    WSAPOLLFD pfd = {.fd = sk->s, .events = events, .revents = 0};
    if (WSAPoll(&pfd, 1, 0) > 0 && pfd.revents) {
        *revents = pfd.revents;
        return 1;
    }
    *revents = 0;
    return 0;
}
