// Network and system-applet services the host does not run, answered as an offline console would: local wireless
// (ldn), sockets, network management (nifm), ssl, server names (nsd), GameShare streaming (streamplay), the network
// clock, the error and keyboard applets, the profanity filter, and a few vi and system calls. Without these the SDK
// asks sm: for the service and the runtime stops with a named fatal (ipc.c).
//
// The answers: every network feature unavailable, every applet logged, no guests.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>
#else
#include <sys/random.h>
#endif
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "ldn.h"
#include "platform/platform.h"

#define RES_UNAVAILABLE 0x2A2u   // the generic failure of a network feature that is not there
#define SK_ENETDOWN 50           // Horizon's BSD errno
#define EAI_FAIL 4

static void nothing(Cpu *c) { (void)c; }
static void ok(Cpu *c) { ret_x(c, 0); }
static void unavailable(Cpu *c) { ret_x(c, RES_UNAVAILABLE); }
static void zero(Cpu *c) { ret_x(c, 0); }

// ---- nn::ldn: local wireless (Link Play) via ldn.c when MTF_NET=ldn, or offline when MTF_NET=offline.
static void ldn_ov_init(Cpu *c) { uint32_t r = ldn_initialize(); rt_log("ldn: Initialize() -> %#x", r); ret_x(c, r); }
static void ldn_ov_fini(Cpu *c) { (void)c; rt_log("ldn: Finalize()"); ldn_finalize(); }
static void ldn_ov_state(Cpu *c) { ret_x(c, ldn_get_state()); }
static void ldn_ov_attach(Cpu *c) {
    rt_log("ldn: AttachStateChangeEvent(%#llx)", (unsigned long long)arg_x(c, 0));
    kern_bind_system_event(c, arg_x(c, 0), 0);
    ldn_attach_state_event(arg_x(c, 0));
}
static void ldn_ov_open_station(Cpu *c) { uint32_t r = ldn_open_station(); rt_log("ldn: OpenStation() -> %#x", r); ret_x(c, r); }
static void ldn_ov_close_station(Cpu *c) { uint32_t r = ldn_close_station(); rt_log("ldn: CloseStation() -> %#x", r); ret_x(c, r); }
static void ldn_ov_open_ap(Cpu *c) { uint32_t r = ldn_open_access_point(); rt_log("ldn: OpenAccessPoint() -> %#x", r); ret_x(c, r); }
static void ldn_ov_close_ap(Cpu *c) { uint32_t r = ldn_close_access_point(); rt_log("ldn: CloseAccessPoint() -> %#x", r); ret_x(c, r); }
static void ldn_ov_disconnect(Cpu *c) { uint32_t r = ldn_disconnect(); rt_log("ldn: Disconnect() -> %#x", r); ret_x(c, r); }
static void ldn_ov_destroy(Cpu *c) { uint32_t r = ldn_destroy_network(); rt_log("ldn: DestroyNetwork() -> %#x", r); ret_x(c, r); }
static void ldn_ov_scan(Cpu *c) {
    int32_t *cnt = arg_p(c, 1);
    uint32_t r = ldn_scan(arg_p(c, 0), cnt, (int32_t)arg_x(c, 4), arg_p(c, 3));
    rt_log("ldn: Scan(max=%d) -> %#x (found=%d)", (int)arg_x(c, 4), r, cnt ? *cnt : -1);
    ret_x(c, r);
}
static void ldn_ov_create(Cpu *c) {
    uint32_t r = ldn_create_network(arg_p(c, 0), arg_p(c, 1), arg_p(c, 2));
    rt_log("ldn: CreateNetwork() -> %#x", r);
    ret_x(c, r);
}
static void ldn_ov_create_priv(Cpu *c) {
    uint32_t r = ldn_create_network(arg_p(c, 0), arg_p(c, 1), arg_p(c, 3));
    rt_log("ldn: CreateNetworkPrivate() -> %#x", r);
    ret_x(c, r);
}
static void ldn_ov_connect(Cpu *c) {
    uint32_t r = ldn_connect(arg_p(c, 0), arg_p(c, 1), arg_p(c, 2));
    rt_log("ldn: Connect() -> %#x", r);
    ret_x(c, r);
}
static void ldn_ov_get_net_info(Cpu *c) { ret_x(c, ldn_get_network_info(arg_p(c, 0))); }
static void ldn_ov_ip(Cpu *c) { ret_x(c, ldn_get_ipv4_address(arg_p(c, 0), arg_p(c, 1))); }
static void ldn_ov_get_sec(Cpu *c) { ret_x(c, ldn_get_security_parameter(arg_p(c, 0))); }
static void ldn_ov_set_adv(Cpu *c) { ret_x(c, ldn_set_advertise_data(arg_p(c, 0), arg_x(c, 1))); }
static void ldn_ov_ok_if_enabled(Cpu *c) { ret_x(c, ldn_enabled() ? LDN_RES_OK : RES_UNAVAILABLE); }

HOST_OVERRIDE("_ZN2nn3ldn10InitializeEv", ldn_ov_init);
HOST_OVERRIDE("_ZN2nn3ldn8FinalizeEv", ldn_ov_fini);
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ldn_ov_state);
HOST_OVERRIDE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_ov_attach);
HOST_OVERRIDE("_ZN2nn3ldn11SetProtocolENS0_8ProtocolE", nothing);
HOST_OVERRIDE("_ZN2nn3ldn11OpenStationEv", ldn_ov_open_station);
HOST_OVERRIDE("_ZN2nn3ldn12CloseStationEv", ldn_ov_close_station);
HOST_OVERRIDE("_ZN2nn3ldn15OpenAccessPointEv", ldn_ov_open_ap);
HOST_OVERRIDE("_ZN2nn3ldn16CloseAccessPointEv", ldn_ov_close_ap);
HOST_OVERRIDE("_ZN2nn3ldn10DisconnectEv", ldn_ov_disconnect);
HOST_OVERRIDE("_ZN2nn3ldn14DestroyNetworkEv", ldn_ov_destroy);
HOST_OVERRIDE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_ov_scan);
HOST_OVERRIDE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_ov_scan);
HOST_OVERRIDE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_ov_create);
HOST_OVERRIDE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
              ldn_ov_create_priv);
HOST_OVERRIDE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_ov_connect);
HOST_OVERRIDE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              unavailable);
HOST_OVERRIDE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_ov_get_net_info);
HOST_OVERRIDE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_ov_ip);
HOST_OVERRIDE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_ov_get_sec);
HOST_OVERRIDE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_ov_set_adv);
HOST_OVERRIDE("_ZN2nn3ldn19GetDisconnectReasonEv", zero);
HOST_OVERRIDE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", ldn_ov_ok_if_enabled);
HOST_OVERRIDE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_ov_ok_if_enabled);
HOST_OVERRIDE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_ov_ok_if_enabled);

// ---- nn::nifm: available when LDN is active, otherwise offline. The request is on hold for the first query after
// a submission, as the service evaluates it asynchronously on the console (network threads rely on that order).
static volatile int32_t nifm_hold;
static void nifm_submit(Cpu *c) { (void)c; rt_log("nifm: SubmitNetworkRequest()"); plat_xchg32(&nifm_hold, 1); }
static void nifm_cancel(Cpu *c) { (void)c; rt_log("nifm: CancelNetworkRequest()"); plat_xchg32(&nifm_hold, 0); }
static void nifm_on_hold(Cpu *c) { ret_x(c, (uint64_t)(plat_xchg32(&nifm_hold, 0) > 0)); }
static void nifm_available(Cpu *c) {
    int a = ldn_nifm_available();
    static int logged = -1;
    if (logged != a) { logged = a; rt_log("nifm: IsNetworkAvailable() -> %d", a); }
    ret_x(c, (uint64_t)a);
}
static void nifm_ip(Cpu *c) {
    if (ldn_enabled()) {
        ret_x(c, ldn_nifm_ip_info(arg_p(c, 0), arg_p(c, 1), arg_p(c, 2), arg_p(c, 3), arg_p(c, 4)));
        return;
    }
    for (int i = 0; i < 5; i++) {
        uint32_t *p = arg_p(c, i);
        if (p) *p = 0;
    }
    ret_x(c, RES_UNAVAILABLE);
}
HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", nothing);
HOST_OVERRIDE("_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_submit);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel);
HOST_OVERRIDE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", nifm_on_hold);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", nifm_ip);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", unavailable);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", zero);   // bool: the system screen resolved nothing

// ---- nn::socket: UDP backend over Winsock2 (ldn.c) when MTF_NET=ldn, plus host EventFd counters.
#define EFD_BASE 0x4000
#define EFD_MAX 64
static struct { volatile int used; volatile uint64_t count; int semaphore; } efd[EFD_MAX];
static PlatMutex efd_lock = PLAT_MUTEX_INIT;
static int efd_index(int fd) { return fd >= EFD_BASE && fd < EFD_BASE + EFD_MAX && efd[fd - EFD_BASE].used ? fd - EFD_BASE : -1; }

static void sk_errno(Cpu *c) { ret_x(c, (uint64_t)(ldn_enabled() ? ldn_sock_last_errno() : SK_ENETDOWN)); }
static void sk_socket(Cpu *c) {
    int fd = ldn_sock_socket((int)arg_x(c, 0), (int)arg_x(c, 1), (int)arg_x(c, 2));
    rt_log("socket: Socket(fam=%d, type=%d, proto=%d) -> %d", (int)arg_x(c, 0), (int)arg_x(c, 1), (int)arg_x(c, 2), fd);
    ret_x(c, (uint64_t)(int64_t)fd);
}
static void sk_bind(Cpu *c) {
    ret_x(c, (uint64_t)(int64_t)ldn_sock_bind((int)arg_x(c, 0), arg_p(c, 1), (uint32_t)arg_x(c, 2)));
}
static void sk_setsockopt(Cpu *c) {
    ret_x(c, (uint64_t)(int64_t)ldn_sock_setsockopt((int)arg_x(c, 0), (int)arg_x(c, 1), (int)arg_x(c, 2),
                                                     arg_p(c, 3), (uint32_t)arg_x(c, 4)));
}
static void sk_sendto(Cpu *c) {
    ret_x(c, (uint64_t)ldn_sock_sendto((int)arg_x(c, 0), arg_p(c, 1), arg_x(c, 2), (int)arg_x(c, 3),
                                       arg_p(c, 4), (uint32_t)arg_x(c, 5)));
}
static void sk_recvfrom(Cpu *c) {
    ret_x(c, (uint64_t)ldn_sock_recvfrom((int)arg_x(c, 0), arg_p(c, 1), arg_x(c, 2), (int)arg_x(c, 3),
                                         arg_p(c, 4), arg_p(c, 5)));
}
static void sk_shutdown(Cpu *c) {
    ret_x(c, (uint64_t)(int64_t)ldn_sock_shutdown((int)arg_x(c, 0), (int)arg_x(c, 1)));
}
static void sk_gai(Cpu *c) {
    void **res = arg_p(c, 3);
    if (res) *res = NULL;
    ret_x(c, EAI_FAIL);
}
static void sk_gai_str(Cpu *c) {
    static char *text;
    if (!text) {
        static const char msg[] = "network unavailable";
        text = mem_kernel_alloc(sizeof msg, 16);
        memcpy(text, msg, sizeof msg);
    }
    ret_x(c, (uint64_t)(uintptr_t)text);
}
// int EventFd(uint64_t initial, EventFdFlags): flag 1 = semaphore
static void sk_eventfd(Cpu *c) {
    plat_mutex_lock(&efd_lock);
    int i = 0;
    while (i < EFD_MAX && efd[i].used) i++;
    if (i < EFD_MAX) efd[i].used = 1, efd[i].count = arg_x(c, 0), efd[i].semaphore = (int)(arg_x(c, 1) & 1);
    plat_mutex_unlock(&efd_lock);
    ret_x(c, i < EFD_MAX ? (uint64_t)(EFD_BASE + i) : (uint64_t)-1);
}
static void sk_eventfd_read(Cpu *c) {
    int i = efd_index((int)arg_x(c, 0));
    uint64_t *out = arg_p(c, 1);
    if (i < 0) { ret_x(c, (uint64_t)-1); return; }
    plat_mutex_lock(&efd_lock);
    uint64_t v = efd[i].count;
    if (v) efd[i].count = efd[i].semaphore ? v - 1 : 0;
    plat_mutex_unlock(&efd_lock);
    if (!v) { ret_x(c, (uint64_t)-1); return; }   // EAGAIN: the game polls before it reads
    if (out) *out = efd[i].semaphore ? 1 : v;
    ret_x(c, 0);
}
static void sk_eventfd_write(Cpu *c) {
    int i = efd_index((int)arg_x(c, 0));
    if (i < 0) { ret_x(c, (uint64_t)-1); return; }
    plat_mutex_lock(&efd_lock);
    efd[i].count += arg_x(c, 1);
    plat_mutex_unlock(&efd_lock);
    ret_x(c, 0);
}
static void sk_close(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    int i = efd_index(fd);
    if (i >= 0) {
        efd[i].used = 0;
        ret_x(c, 0);
    } else if (ldn_sock_is_fd(fd)) {
        ret_x(c, (uint64_t)(int64_t)ldn_sock_close(fd));
    } else {
        ret_x(c, (uint64_t)-1);
    }
}
// int Poll(pollfd *, nfds, timeout ms): checks both event descriptors and LDN UDP sockets.
typedef struct { int32_t fd; int16_t events, revents; } PollFd;
static void sk_poll(Cpu *c) {
    PollFd *p = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int32_t timeout = (int32_t)arg_x(c, 2);
    uint64_t deadline = kern_now_ns() + (uint64_t)(timeout < 0 ? 100 : timeout) * 1000000ull;
    for (;;) {
        int ready = 0;
        for (uint64_t k = 0; p && k < n; k++) {
            int i = efd_index(p[k].fd);
            if (i >= 0) {
                p[k].revents = (int16_t)(efd[i].count && (p[k].events & 1) ? 1 : 0);
                ready += p[k].revents != 0;
            } else {
                int16_t rev = 0;
                if (ldn_sock_poll_ready(p[k].fd, p[k].events, &rev)) {
                    p[k].revents = rev;
                    ready += rev != 0;
                } else {
                    p[k].revents = 0;
                }
            }
        }
        uint64_t now = kern_now_ns();
        if (ready || now >= deadline) { ret_x(c, (uint64_t)ready); return; }
        int away = kern_core_away();
        plat_wait_ns(deadline - now < 2000000 ? deadline - now : 2000000);
        kern_core_back(&away);
    }
}
HOST_OVERRIDE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ok);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", ok);
static void one(Cpu *c) { ret_x(c, 1); }
HOST_OVERRIDE("_ZN2nn6socket13IsInitializedEv", one);   // bool: Initialize succeeded
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", sk_errno);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrorEv", sk_errno);
HOST_OVERRIDE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", sk_socket);
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", sk_socket);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", sk_bind);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", sk_bind);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", sk_setsockopt);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", sk_sendto);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", sk_sendto);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", sk_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", sk_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", sk_shutdown);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", sk_close);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", sk_poll);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", sk_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", sk_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", sk_eventfd_write);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", sk_gai);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", sk_gai);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", nothing);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", nothing);
HOST_OVERRIDE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", sk_gai_str);

// ---- nn::ssl: the library and contexts succeed, connecting to nothing; imports and certificates fail
static void ssl_import(Cpu *c) {
    uint64_t *id = arg_p(c, 1);
    if (id) *id = 0;
    ret_x(c, RES_UNAVAILABLE);
}
static void ssl_certs(Cpu *c) {
    void **out = arg_p(c, 0);
    if (out) *out = NULL;
    ret_x(c, RES_UNAVAILABLE);
}
static void ssl_cert_size(Cpu *c) {
    uint32_t *n = arg_p(c, 0);
    if (n) *n = 0;
    ret_x(c, RES_UNAVAILABLE);
}
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ssl_certs);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_cert_size);

// ---- nn::nsd: the retail environment "lp1"; no name resolves
static void nsd_env(Cpu *c) {
    char *out = arg_p(c, 0);
    memset(out, 0, 8);
    memcpy(out, "lp1", 3);
}
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_env);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", unavailable);

// ---- nn::streamplay (GameShare): ready, with no session and no guest
static uint64_t stpl_event;
static void stpl_mem_size(Cpu *c) { ret_x(c, 0x1000); }
static void stpl_conn_event(Cpu *c) {
    if (!stpl_event) stpl_event = (uint64_t)(uintptr_t)mem_kernel_alloc(0x80, 16);
    kern_bind_system_event(c, stpl_event, 0);
    ret_x(c, stpl_event);
}
static void stpl_conn_state(Cpu *c) { *(uint32_t *)arg_p(c, 0) = 0; }
HOST_OVERRIDE("_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi", stpl_mem_size);
HOST_OVERRIDE("_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", ok);
HOST_OVERRIDE("_ZN2nn10streamplay8FinalizeEv", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay24CreateIndividualDisplaysEi", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay25DestroyIndividualDisplaysEv", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay30GetConnectionStateChangedEventEv", stpl_conn_event);
HOST_OVERRIDE("_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", stpl_conn_state);
HOST_OVERRIDE("_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay25SetSupportedHidFeatureSetENS_4util10BitFlagSetILi32ENS0_13HidFeatureTagEEE", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay28GetGuestSixAxisSensorHandlesEPNS0_24GuestSixAxisSensorHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE",
              zero);
HOST_OVERRIDE("_ZN2nn10streamplay27GetGuestSixAxisSensorStatesEPNS0_23GuestSixAxisSensorStateEiRKNS0_24GuestSixAxisSensorHandleE", zero);
HOST_OVERRIDE("_ZN2nn10streamplay31OpenSessionWithMatchmakeSupportEPNS0_11SessionInfoERKNS0_15MatchmakeConfigE", unavailable);
HOST_OVERRIDE("_ZN2nn10streamplay14StartStreamingEv", unavailable);
HOST_OVERRIDE("_ZN2nn10streamplay12CloseSessionEv", nothing);
HOST_OVERRIDE("_ZN2nn10streamplay21WriteGuestAudioStreamENS0_7GuestIdEPKvi", unavailable);

// ---- nn::time: the network clock is never accurate; ensuring it finishes at once and fails. The task's finish event
// lives on the host, one per task object.
#define TASKS 8
static struct { uint64_t obj, event; } tasks[TASKS];
static PlatMutex task_lock = PLAT_MUTEX_INIT;
static uint64_t task_event(Cpu *c, uint64_t obj) {
    plat_mutex_lock(&task_lock);
    int i = 0;
    while (i < TASKS && tasks[i].obj && tasks[i].obj != obj) i++;
    if (i == TASKS) rt_fatal("time: more than %d network clock tasks", TASKS);
    if (!tasks[i].obj) tasks[i].obj = obj, tasks[i].event = (uint64_t)(uintptr_t)mem_kernel_alloc(0x80, 16);
    uint64_t ev = tasks[i].event;
    plat_mutex_unlock(&task_lock);
    kern_bind_system_event(c, ev, 0);
    return ev;
}
static void task_init(Cpu *c) { task_event(c, arg_x(c, 0)); ret_x(c, 0); }
static void task_get_event(Cpu *c) { ret_x(c, task_event(c, arg_x(c, 0))); }
static void task_start(Cpu *c) { kern_signal_system_event(task_event(c, arg_x(c, 0))); ret_x(c, 0); }
static void location_name(Cpu *c) {
    char *out = arg_p(c, 0);
    memset(out, 0, 36);
    memcpy(out, "UTC", 3);
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", zero);
HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", location_name);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", nothing);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", nothing);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", task_init);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", task_get_event);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", unavailable);

// ---- applets: the error applet is logged, the keyboard is cancelled, the profanity filter finds nothing
static void err_show_variant(Cpu *c) { (void)c; rt_log("[RED] err: the game asks to show an error (ErrorResultVariant)"); }
static void swkbd_show(Cpu *c) {
    (void)c;
    rt_log("[RED] swkbd: the game opens the software keyboard; the host cancels it");
    ret_x(c, 0x29du);   // nn::swkbd::ResultCanceled (module 157, description 1)
}
HOST_OVERRIDE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", err_show_variant);
HOST_OVERRIDE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show);
// Let sdk's real ProfanityFilter::ProfanityFilter() run so the object's vtable is populated (required by ~ProfanityFilter()).
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ok);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ok);

// ---- nn::vi: closing and querying the one display
// BufferMaxResolution is an enum, 0 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160:
// the largest display buffer a docked Switch 2 says the TV takes. This answered two ints,
// 1920 and 1080, an enum value no table holds. The answer is the output size ov_resolution.c decides (MTF_OUTPUT_RES:
// 720, 1080, 1440 or 2160 lines, auto by the display); the game caps its scene at it (logged by nvnWindowInitialize).
void ov_resolution_apply(Cpu *c);   // ov_resolution.c
int ov_resolution_lines(void);
static void vi_max_resolution(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    int r = ov_resolution_lines();
    uint32_t e = r >= 2160 ? 3 : r >= 1440 ? 2 : r >= 1080 ? 1 : 0;
    static int once;
    if (!once++) rt_log("vi: QueryDisplayBufferMaxResolution: %u (%s)", e, e == 3 ? "3840x2160" : e == 2 ? "2560x1440"
                                                                    : e == 1 ? "1920x1080" : "1280x720");
    if (out) *out = e;
    ov_resolution_apply(c);
    ret_x(c, 0);
}

// The display's HDR (Switch 2 SDK): the game asks whether the TV is in HDR, its output format and peak, and the paper
// white, and gives each layer its HDR metadata (content gamut, luminance ranges, HDR-to-SDR tone mapping, a Bezier
// curve). MTF_HDR=1 answers HDR on, so the game renders its own HDR picture (the present path then needs the HDR10
// swapchain, vk_hdr); otherwise SDR, as before. The metadata calls are logged once each and otherwise succeed with
// nothing to apply: unanswered, the SDK forwards them through a table its graphics system fills, null here: a call through it
// would jump to null.
static void vi_hdr_status(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    uint32_t *fmt = arg_p(c, 1);
    uint16_t *peak = arg_p(c, 2);
    int hdr = cfg_bool("MTF_HDR");
    int nits = cfg_int("MTF_HDR_PEAK_NITS");
    if (nits < 400 || nits > 10000) nits = 1000;
    static int once;
    if (!once++) rt_log("vi: QueryDisplayHdrStatus: HDR %s (format %d, peak %d nits)", hdr ? "on" : "off", hdr ? 1 : 0, hdr ? nits : 0);
    if (on) *on = (uint8_t)hdr;
    if (fmt) *fmt = hdr ? 1 : 0;   // 0 SDR; 1 taken as HDR10, to be checked against what the game does
    if (peak) *peak = (uint16_t)(hdr ? nits : 0);
    ret_x(c, 0);
}
static void vi_hdr_paper_white(Cpu *c) {
    uint16_t *out = arg_p(c, 0);
    int nits = cfg_int("MTF_HDR_PAPER_WHITE");
    if (nits < 80 || nits > 1000) nits = 203;   // BT.2408's reference white
    if (out) *out = (uint16_t)nits;
    ret_x(c, 0);
}
static void vi_hdr_meta(Cpu *c) {   // (layer, values...): x1.. integers, s0.. floats
    static int logged[8];
    uint64_t lr = c->x[30];
    int k = (int)(lr % 8);
    if (logged[k]++ < 2)
        rt_log("vi: layer HDR metadata from %#llx: %#llx %#llx %#llx / %g %g %g", (unsigned long long)lr,
               (unsigned long long)arg_x(c, 1), (unsigned long long)arg_x(c, 2), (unsigned long long)arg_x(c, 3),
               (double)arg_s(c, 0), (double)arg_s(c, 1), (double)arg_s(c, 2));
    ret_x(c, 0);
}
static void vi_vrr_supported(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    if (on) *on = 0;
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", vi_hdr_status);
HOST_OVERRIDE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", vi_hdr_paper_white);
HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", vi_vrr_supported);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", ok);
HOST_OVERRIDE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", nothing);
HOST_OVERRIDE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", nothing);
HOST_OVERRIDE("_ZN2nn2vi8FinalizeEv", nothing);
HOST_OVERRIDE("_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", vi_max_resolution);

// ---- system: random bytes from the host, shared font loads (the fonts are read on first use, ov_system.c), and
// MountRom without its cache arguments
static void random_bytes(Cpu *c) {
#ifdef _WIN32
    if (BCryptGenRandom(NULL, arg_p(c, 0), (ULONG)arg_x(c, 1), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        rt_fatal("crypto: BCryptGenRandom failed");
#else
    uint8_t *dst = (uint8_t *)arg_p(c, 0);
    size_t rem = (size_t)arg_x(c, 1);
    while (rem > 0) {
        ssize_t n = getrandom(dst, rem, 0);
        if (n <= 0) rt_fatal("crypto: getrandom failed");
        dst += n;
        rem -= (size_t)n;
    }
#endif
}
static void mount_rom_1(Cpu *c) {
    GuestFn fn = hostfn_override("_ZN2nn2fs8MountRomEPKcPvm");
    if (!fn) rt_fatal("fs: MountRom(name) without the host's MountRom(name, cache, size)");
    c->x[1] = 0, c->x[2] = 0;
    fn(c);
}
HOST_OVERRIDE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", random_bytes);
HOST_OVERRIDE("_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", nothing);
HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKc", mount_rom_1);
