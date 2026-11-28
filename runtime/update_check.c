// Background GitHub release check for mtf-recomp.exe (non-blocking, silent on network errors).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "update_check.h"

#define RELEASES_URL "https://github.com/rafael-fj/mtf-recompiled/releases/latest"
#define MAX_API_JSON (256 * 1024)

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static UpdateInfo g_info;
static int g_started;
static char g_notes[8192];

static void parse_semver(const char *s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    while (*s && !isdigit((unsigned char)*s)) s++;
    for (int i = 0; i < 3 && *s; i++) {
        out[i] = (int)strtol(s, (char **)&s, 10);
        if (*s == '.') s++;
        else break;
    }
}

static int is_newer(const char *candidate, const char *current) {
    int a[3], b[3];
    parse_semver(candidate, a);
    parse_semver(current, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 0;
}

static int json_str_field(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static void json_body_field(const char *json, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 3 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p++;
            if (c == 'n') {
                if (n == 0 || out[n - 1] != '\r') out[n++] = '\r';
                out[n++] = '\n';
            } else if (c == 'r') {
                out[n++] = '\r';
            } else if (c == 't') {
                out[n++] = ' ';
                out[n++] = ' ';
            } else {
                out[n++] = c;
            }
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
}

static int json_u64_field(const char *json, const char *key, uint64_t *out) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    *out = (uint64_t)strtoull(p, NULL, 10);
    return 1;
}

static int has_game_dll_beside_exe(void) {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    char *s = strrchr(path, '\\');
    char *f = strrchr(path, '/');
    if (f && (!s || f > s)) s = f;
    if (!s) return 0;
    snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "mtf-game.dll");
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return 0;
#endif
}

static void find_windows_asset(const char *json, char *dl_url, size_t dl_cap, uint64_t *size_out) {
    dl_url[0] = 0;
    *size_out = 0;
    int prefer_runtime = has_game_dll_beside_exe();
    char full_url[512] = {0};
    uint64_t full_sz = 0;

    const char *cur = json;
    while ((cur = strstr(cur, "\"browser_download_url\"")) != NULL) {
        char candidate[512] = {0};
        if (json_str_field(cur, "browser_download_url", candidate, sizeof candidate)) {
            int is_runtime = (strstr(candidate, "internal-updater-") && strstr(candidate, "-win64.zip")) ||
                             (strstr(candidate, "mtf-internal-ingame-rt-") && strstr(candidate, "-win64.zip")) ||
                             (strstr(candidate, "MTF-Runtime-") && strstr(candidate, "Windows-x64.zip"));
            int is_full = strstr(candidate, "Windows-x64.zip") && !is_runtime;
            if (is_runtime || is_full) {
                const char *back = cur - json > 1024 ? cur - 1024 : json;
                const char *sz = NULL;
                for (const char *q = back; q < cur; q++) {
                    if (!strncmp(q, "\"size\"", 6)) sz = q;
                }
                uint64_t sz_val = 0;
                if (sz) json_u64_field(sz, "size", &sz_val);
                if (is_runtime && prefer_runtime) {
                    snprintf(dl_url, dl_cap, "%s", candidate);
                    *size_out = sz_val;
                    return;
                }
                if (is_full && !full_url[0]) {
                    snprintf(full_url, sizeof full_url, "%s", candidate);
                    full_sz = sz_val;
                }
            }
        }
        cur += 22;
    }
    if (full_url[0]) {
        snprintf(dl_url, dl_cap, "%s", full_url);
        *size_out = full_sz;
    }
}

static void set_available(const char *tag, const char *url, const char *dl_url, uint64_t size) {
    plat_mutex_lock(&g_lock);
    g_info.available = 1;
    snprintf(g_info.tag, sizeof g_info.tag, "%s", tag);
    snprintf(g_info.url, sizeof g_info.url, "%s", url && *url ? url : RELEASES_URL);
    if (dl_url && *dl_url) snprintf(g_info.download_url, sizeof g_info.download_url, "%s", dl_url);
    g_info.bytes_total = size;
    plat_mutex_unlock(&g_lock);
    rt_log("update_check: new release %s available (current %s)", tag, rt_port_version);
}

#ifdef _WIN32
static void update_worker(void *arg) {
    (void)arg;
    HINTERNET ses = WinHttpOpen(L"MTF-Recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return;
    WinHttpSetTimeouts(ses, 4000, 4000, 4000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/repos/rafael-fj/mtf-recompiled/releases/latest", NULL,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return;
    }
    const wchar_t *hdr = L"Accept: application/vnd.github+json\r\n";
    BOOL ok = WinHttpSendRequest(req, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, NULL);
    DWORD status = 0, slen = sizeof status;
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &slen, NULL);
    if (ok && status == 200) {
        char *buf = (char *)malloc(MAX_API_JSON);
        if (buf) {
            size_t total = 0;
            DWORD avail = 0;
            while (total + 1 < MAX_API_JSON && WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                DWORD to_read = avail;
                if (total + to_read >= MAX_API_JSON) to_read = (DWORD)(MAX_API_JSON - 1 - total);
                DWORD got = 0;
                if (!WinHttpReadData(req, buf + total, to_read, &got) || !got) break;
                total += got;
            }
            buf[total] = 0;
            char tag[32] = {0}, url[256] = {0}, dl[512] = {0};
            uint64_t sz = 0;
            if (json_str_field(buf, "tag_name", tag, sizeof tag) && is_newer(tag, rt_port_version)) {
                json_str_field(buf, "html_url", url, sizeof url);
                find_windows_asset(buf, dl, sizeof dl, &sz);
                plat_mutex_lock(&g_lock);
                json_body_field(buf, "body", g_notes, sizeof g_notes);
                plat_mutex_unlock(&g_lock);
                set_available(tag, url, dl, sz);
            }
            free(buf);
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
}
#else
typedef struct {
    char buf[16384];
    size_t len;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *b = (CurlBuf *)userdata;
    size_t n = size * nmemb;
    size_t avail = sizeof b->buf - 1 - b->len;
    size_t take = n < avail ? n : avail;
    if (take > 0) {
        memcpy(b->buf + b->len, ptr, take);
        b->len += take;
        b->buf[b->len] = 0;
    }
    return n;
}

static void update_worker(void *arg) {
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) return;
    CurlBuf b = {0};
    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/repos/rafael-fj/mtf-recompiled/releases/latest");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MTF-Recomp/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (res == CURLE_OK && status == 200) {
        char tag[32] = {0}, url[256] = {0}, dl[512] = {0};
        uint64_t sz = 0;
        if (json_str_field(b.buf, "tag_name", tag, sizeof tag) && is_newer(tag, rt_port_version)) {
            json_str_field(b.buf, "html_url", url, sizeof url);
            find_windows_asset(b.buf, dl, sizeof dl, &sz);
            set_available(tag, url, dl, sz);
        }
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
}
#endif

void update_check_start(void) {
    plat_mutex_lock(&g_lock);
    if (g_started) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_started = 1;
    plat_mutex_unlock(&g_lock);

    const char *e = cfg_get("MTF_UPDATE_CHECK");
    if (e && !strcmp(e, "0")) return;

    const char *test_tag = cfg_get("MTF_UPDATE_TEST_TAG");
    if (test_tag && *test_tag) {
        if (is_newer(test_tag, rt_port_version)) set_available(test_tag, RELEASES_URL, NULL, 0);
        return;
    }

    plat_thread_spawn(update_worker, NULL, "update-check", 0);
}

int update_check_status(UpdateInfo *out) {
    plat_mutex_lock(&g_lock);
    if (out) *out = g_info;
    int avail = g_info.available;
    plat_mutex_unlock(&g_lock);
    return avail;
}

void update_check_open_url(void) {
    UpdateInfo info;
    if (!update_check_status(&info)) return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", info.url, NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", info.url, (char *)NULL);
        _exit(127);
    }
#endif
}

#ifdef _WIN32
static int exe_dir(char *out, size_t cap) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return 0;
    char *s = strrchr(out, '\\');
    char *f = strrchr(out, '/');
    if (f && (!s || f > s)) s = f;
    if (!s) return 0;
    *s = 0;
    return 1;
}

static void set_err(const char *msg) {
    plat_mutex_lock(&g_lock);
    g_info.state = UPDATE_STATE_ERROR;
    snprintf(g_info.error, sizeof g_info.error, "%s", msg);
    plat_mutex_unlock(&g_lock);
    rt_log("update_check: %s", msg);
}

static void install_worker(void *arg) {
    (void)arg;
    UpdateInfo info;
    update_check_status(&info);
    if (!info.download_url[0]) {
        set_err("No Windows release package URL found.");
        return;
    }

    char dir[MAX_PATH], temp_dir[MAX_PATH], zip_path[MAX_PATH], src_upd[MAX_PATH], tmp_upd[MAX_PATH];
    if (!exe_dir(dir, sizeof dir)) {
        set_err("Could not resolve executable directory.");
        return;
    }
    snprintf(temp_dir, sizeof temp_dir, "%s\\temp", dir);
    CreateDirectoryA(temp_dir, NULL);
    snprintf(zip_path, sizeof zip_path, "%s\\update.zip", temp_dir);
    snprintf(src_upd, sizeof src_upd, "%s\\mtf-updater.exe", dir);
    snprintf(tmp_upd, sizeof tmp_upd, "%s\\mtf-updater.exe", temp_dir);
    if (!CopyFileA(src_upd, tmp_upd, FALSE)) {
        set_err("mtf-updater.exe not found next to mtf-recomp.exe.");
        return;
    }

    wchar_t wurl[1024] = {0};
    MultiByteToWideChar(CP_UTF8, 0, info.download_url, -1, wurl, 1024);
    URL_COMPONENTS uc = {0};
    wchar_t host[256] = {0}, path[768] = {0};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 768;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        set_err("Invalid download URL.");
        return;
    }

    HINTERNET ses = WinHttpOpen(L"MTF-Recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { set_err("WinHttpOpen failed."); return; }
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) { WinHttpCloseHandle(ses); set_err("Connection failed."); return; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); set_err("Request failed."); return; }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, NULL);
    DWORD status = 0, slen = sizeof status;
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &slen, NULL);
    if (!ok || status != 200) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        set_err("Download failed (HTTP error).");
        return;
    }

    DWORD clen_len = 0;
    wchar_t clen_buf[32] = {0};
    DWORD clen_bytes = sizeof clen_buf;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, NULL, clen_buf, &clen_bytes, &clen_len)) {
        uint64_t hdr_size = (uint64_t)_wcstoui64(clen_buf, NULL, 10);
        if (hdr_size > 0) {
            plat_mutex_lock(&g_lock);
            g_info.bytes_total = hdr_size;
            plat_mutex_unlock(&g_lock);
        }
    }

    FILE *fp = fopen(zip_path, "wb");
    if (!fp) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        set_err("Could not create temp/update.zip.");
        return;
    }

    uint8_t *chunk = (uint8_t *)malloc(65536);
    if (!chunk) {
        fclose(fp);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        set_err("Out of memory.");
        return;
    }

    uint64_t done = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = FALSE; break; }
        if (avail == 0) break;
        DWORD want = avail > 65536 ? 65536 : avail;
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk, want, &got) || got == 0) break;
        if (fwrite(chunk, 1, got, fp) != got) { ok = FALSE; break; }
        done += got;
        plat_mutex_lock(&g_lock);
        g_info.bytes_done = done;
        plat_mutex_unlock(&g_lock);
    }
    free(chunk);
    fclose(fp);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);

    if (!ok || done == 0) {
        DeleteFileA(zip_path);
        set_err("Download interrupted.");
        return;
    }

    plat_mutex_lock(&g_lock);
    g_info.state = UPDATE_STATE_APPLYING;
    plat_mutex_unlock(&g_lock);

    cfg_save();
    playtime_save();

    char notes_path[MAX_PATH] = {0};
    plat_mutex_lock(&g_lock);
    if (g_notes[0]) {
        snprintf(notes_path, sizeof notes_path, "%s\\patch-notes.txt", temp_dir);
        FILE *nf = fopen(notes_path, "wb");
        if (nf) {
            fprintf(nf, "Release %s\r\n\r\n%s", info.tag, g_notes);
            fclose(nf);
        } else {
            notes_path[0] = 0;
        }
    }
    plat_mutex_unlock(&g_lock);

    char cmd[2560];
    if (notes_path[0]) {
        snprintf(cmd, sizeof cmd, "\"%s\" --pid %lu --dest \"%s\" --zip \"%s\" --notes \"%s\"",
                 tmp_upd, (unsigned long)GetCurrentProcessId(), dir, zip_path, notes_path);
    } else {
        snprintf(cmd, sizeof cmd, "\"%s\" --pid %lu --dest \"%s\" --zip \"%s\"",
                 tmp_upd, (unsigned long)GetCurrentProcessId(), dir, zip_path);
    }
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    if (!CreateProcessA(tmp_upd, cmd, NULL, NULL, FALSE, 0, NULL, temp_dir, &si, &pi)) {
        set_err("Failed to launch mtf-updater.exe.");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

int update_check_can_install(void) {
    UpdateInfo info;
    if (!update_check_status(&info) || !info.download_url[0]) return 0;
    char dir[MAX_PATH], upd[MAX_PATH];
    if (!exe_dir(dir, sizeof dir)) return 0;
    snprintf(upd, sizeof upd, "%s\\mtf-updater.exe", dir);
    DWORD attr = GetFileAttributesA(upd);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void update_check_start_install(void) {
    plat_mutex_lock(&g_lock);
    if (!g_info.available || g_info.state == UPDATE_STATE_DOWNLOADING || g_info.state == UPDATE_STATE_APPLYING) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_info.state = UPDATE_STATE_DOWNLOADING;
    g_info.bytes_done = 0;
    g_info.error[0] = 0;
    plat_mutex_unlock(&g_lock);
    plat_thread_spawn(install_worker, NULL, "update-install", 0);
}
#else
int update_check_can_install(void) { return 0; }
void update_check_start_install(void) {}
#endif
