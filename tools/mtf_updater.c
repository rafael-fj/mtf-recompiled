// Standalone Windows GUI self-updater helper for MTF Recomp.
// Displays a clean native progress window with release patch notes while waiting for
// mtf-recomp.exe to exit, extracting the update archive, applying either a fast runtime
// update or full incremental build (preserving profile/, mtf.ini, game/, nvngx_dlss.dll),
// and relaunching the game.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_UPD_PROGRESS (WM_APP + 1)
#define WM_UPD_DONE     (WM_APP + 2)

typedef struct {
    DWORD pid;
    char dest[MAX_PATH];
    char zip_path[MAX_PATH];
    char notes_path[MAX_PATH];
    HWND hwnd;
    HWND edit_notes;
    HWND lbl_status;
    HWND lbl_pct;
    HWND prog;
    int exit_code;
    char err_msg[512];
} UpdaterCtx;

static UpdaterCtx g_ctx;
static FILE *g_log;

static void log_line(const char *s) {
    if (g_log) {
        fprintf(g_log, "%s\n", s);
        fflush(g_log);
    }
}

static void set_ui(int pct, const char *status) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char *msg = _strdup(status ? status : "");
    PostMessageA(g_ctx.hwnd, WM_UPD_PROGRESS, (WPARAM)pct, (LPARAM)msg);
}

static int dir_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int run_silent(const char *cmd, const char *cwd) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    char *mut = _strdup(cmd);
    if (!mut) return -1;
    BOOL ok = CreateProcessA(NULL, mut, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    free(mut);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void rm_tree(const char *path) {
    if (!dir_exists(path)) return;
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "cmd.exe /c rmdir /s /q \"%s\"", path);
    run_silent(cmd, NULL);
}

static void find_extracted_root(const char *stage, char *out, size_t cap) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", stage);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int dirs = 0, files = 0;
    char single_dir[MAX_PATH] = {0};
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dirs++;
                if (dirs == 1) snprintf(single_dir, sizeof single_dir, "%s\\%s", stage, fd.cFileName);
            } else {
                files++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (dirs == 1 && files == 0 && single_dir[0]) snprintf(out, cap, "%s", single_dir);
    else snprintf(out, cap, "%s", stage);
}

static int json_str(const char *line, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static int json_int(const char *line, const char *key, int *out) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    *out = atoi(p);
    return 1;
}

static const char *friendly_step(const char *step) {
    if (!strcmp(step, "check")) return "Checking system and game files...";
    if (!strcmp(step, "extract")) return "Extracting game files...";
    if (!strcmp(step, "fonts")) return "Preparing system fonts...";
    if (!strcmp(step, "lift")) return "Translating game code (main/sdk)...";
    if (!strcmp(step, "capture")) return "Capturing dynamic module (nro)...";
    if (!strcmp(step, "lift-nro")) return "Translating dynamic module...";
    if (!strcmp(step, "shaders")) return "Translating shaders...";
    if (!strcmp(step, "build")) return "Building updated executable...";
    if (!strcmp(step, "dlss")) return "Checking DLSS library...";
    if (!strcmp(step, "finish")) return "Finalizing update...";
    return "Applying update...";
}

static int run_installer_with_progress(const char *cmd, const char *cwd) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char *mut = _strdup(cmd);
    BOOL ok = mut && CreateProcessA(NULL, mut, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    free(mut);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }

    char line[4096];
    size_t lpos = 0;
    char buf[1024];
    DWORD got = 0;
    int total_steps = 8;
    int step_idx = 0;
    char cur_step[32] = "check";

    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0) {
        for (DWORD i = 0; i < got; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r') {
                if (lpos > 0) {
                    line[lpos] = 0;
                    log_line(line);
                    char typ[32] = {0};
                    if (json_str(line, "type", typ, sizeof typ)) {
                        if (!strcmp(typ, "plan")) {
                            const char *r = strstr(line, "\"run\"");
                            if (r) {
                                int cnt = 0;
                                const char *q = strchr(r, '[');
                                const char *e = q ? strchr(q, ']') : NULL;
                                if (q && e) {
                                    for (const char *p = q; p < e; p++)
                                        if (*p == ',') cnt++;
                                    if (e > q + 1) total_steps = cnt + 1;
                                }
                            }
                        } else if (!strcmp(typ, "step")) {
                            char st[32] = {0}, step[32] = {0};
                            json_str(line, "step", step, sizeof step);
                            json_str(line, "state", st, sizeof st);
                            if (step[0]) snprintf(cur_step, sizeof cur_step, "%s", step);
                            if (!strcmp(st, "started")) {
                                int pct = 25 + (step_idx * 70) / (total_steps > 0 ? total_steps : 1);
                                set_ui(pct, friendly_step(cur_step));
                            } else if (!strcmp(st, "done") || !strcmp(st, "skipped")) {
                                step_idx++;
                                int pct = 25 + (step_idx * 70) / (total_steps > 0 ? total_steps : 1);
                                set_ui(pct, friendly_step(cur_step));
                            }
                        } else if (!strcmp(typ, "progress")) {
                            int done = 0, total = 0;
                            if (json_int(line, "done", &done) && json_int(line, "total", &total) && total > 0) {
                                double frac = (double)done / (double)total;
                                if (frac > 1.0) frac = 1.0;
                                int pct = 25 + (int)(((step_idx + frac) * 70.0) / (double)(total_steps > 0 ? total_steps : 1));
                                set_ui(pct, friendly_step(cur_step));
                            }
                        }
                    }
                    lpos = 0;
                }
            } else if (lpos + 1 < sizeof line) {
                line[lpos++] = ch;
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static DWORD WINAPI worker_thread(LPVOID param) {
    (void)param;
    char log_dir[MAX_PATH], log_path[MAX_PATH];
    snprintf(log_dir, sizeof log_dir, "%s\\logs", g_ctx.dest);
    CreateDirectoryA(log_dir, NULL);
    snprintf(log_path, sizeof log_path, "%s\\updater.log", log_dir);
    g_log = fopen(log_path, "w");
    log_line("=== MTF Recomp Updater ===");

    set_ui(5, "Waiting for game to close...");
    if (g_ctx.pid) {
        HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, g_ctx.pid);
        if (hp) {
            WaitForSingleObject(hp, 15000);
            CloseHandle(hp);
        }
        Sleep(400);
    }

    char stage[MAX_PATH], work[MAX_PATH];
    snprintf(stage, sizeof stage, "%s\\temp\\update-stage", g_ctx.dest);
    snprintf(work, sizeof work, "%s\\temp\\update-work", g_ctx.dest);
    rm_tree(stage);
    CreateDirectoryA(stage, NULL);

    set_ui(15, "Extracting update package...");
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "tar.exe -xf \"%s\" -C \"%s\"", g_ctx.zip_path, stage);
    int rc = run_silent(cmd, g_ctx.dest);
    if (rc != 0) {
        snprintf(cmd, sizeof cmd,
                 "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
                 "\"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"",
                 g_ctx.zip_path, stage);
        rc = run_silent(cmd, g_ctx.dest);
    }
    if (rc != 0) {
        snprintf(g_ctx.err_msg, sizeof g_ctx.err_msg, "Failed to extract update.zip.");
        g_ctx.exit_code = 1;
        if (g_log) fclose(g_log);
        PostMessageA(g_ctx.hwnd, WM_UPD_DONE, 0, 0);
        return 1;
    }

    char root[MAX_PATH];
    find_extracted_root(stage, root, sizeof root);

    char manifest[MAX_PATH];
    snprintf(manifest, sizeof manifest, "%s\\mtf-runtime-manifest.json", root);
    if (file_exists(manifest)) {
        set_ui(55, "Applying fast runtime update...");
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s\\*", root);
        WIN32_FIND_DATAA fd;
        HANDLE hf = FindFirstFileA(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!_stricmp(fd.cFileName, "mtf-game.dll") || !_stricmp(fd.cFileName, "nvngx_dlss.dll") ||
                    !_stricmp(fd.cFileName, "mtf.ini") || !_stricmp(fd.cFileName, "mtf-runtime-manifest.json"))
                    continue;
                char src[MAX_PATH], dst[MAX_PATH];
                snprintf(src, sizeof src, "%s\\%s", root, fd.cFileName);
                snprintf(dst, sizeof dst, "%s\\%s", g_ctx.dest, fd.cFileName);
                if (!CopyFileA(src, dst, FALSE)) {
                    FindClose(hf);
                    snprintf(g_ctx.err_msg, sizeof g_ctx.err_msg, "Failed to copy %s.", fd.cFileName);
                    g_ctx.exit_code = 1;
                    if (g_log) fclose(g_log);
                    PostMessageA(g_ctx.hwnd, WM_UPD_DONE, 0, 0);
                    return 1;
                }
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
    } else {
        char py_exe[MAX_PATH], setup_exe[MAX_PATH] = {0};
        char cand1[MAX_PATH], cand2[MAX_PATH];
        snprintf(py_exe, sizeof py_exe, "%s\\python\\python.exe", root);
        snprintf(cand1, sizeof cand1, "%s\\MTF-Recomp-Setup\\MTF-Recomp-Setup.exe", root);
        snprintf(cand2, sizeof cand2, "%s\\MTF-Recomp-Setup.exe", root);
        if (file_exists(cand1)) snprintf(setup_exe, sizeof setup_exe, "%s", cand1);
        else if (file_exists(cand2)) snprintf(setup_exe, sizeof setup_exe, "%s", cand2);

        if (!file_exists(py_exe) && !setup_exe[0]) {
            snprintf(g_ctx.err_msg, sizeof g_ctx.err_msg, "Installer not found inside update archive.");
            g_ctx.exit_code = 1;
            if (g_log) fclose(g_log);
            PostMessageA(g_ctx.hwnd, WM_UPD_DONE, 0, 0);
            return 1;
        }

        set_ui(25, "Preparing update...");
        if (file_exists(py_exe)) {
            snprintf(cmd, sizeof cmd, "\"%s\" -m installer --dest \"%s\" --root \"%s\" --temp \"%s\"",
                     py_exe, g_ctx.dest, root, work);
        } else {
            snprintf(cmd, sizeof cmd, "\"%s\" --cli --dest \"%s\" --root \"%s\" --temp \"%s\"",
                     setup_exe, g_ctx.dest, root, work);
        }
        rc = run_installer_with_progress(cmd, root);
        if (rc != 0) {
            snprintf(g_ctx.err_msg, sizeof g_ctx.err_msg,
                     "Update build failed (code %d). See logs\\updater.log for details.", rc);
            g_ctx.exit_code = rc;
            if (g_log) fclose(g_log);
            PostMessageA(g_ctx.hwnd, WM_UPD_DONE, 0, 0);
            return 1;
        }

        char upd1[MAX_PATH], upd2[MAX_PATH], dst_upd[MAX_PATH];
        snprintf(upd1, sizeof upd1, "%s\\mtf-updater.exe", root);
        snprintf(upd2, sizeof upd2, "%s\\deps\\bin\\mtf-updater.exe", root);
        snprintf(dst_upd, sizeof dst_upd, "%s\\mtf-updater.exe", g_ctx.dest);
        if (file_exists(upd1)) CopyFileA(upd1, dst_upd, FALSE);
        else if (file_exists(upd2)) CopyFileA(upd2, dst_upd, FALSE);
    }

    set_ui(96, "Cleaning up temporary files...");
    DeleteFileA(g_ctx.zip_path);
    if (g_ctx.notes_path[0]) DeleteFileA(g_ctx.notes_path);
    rm_tree(stage);
    rm_tree(work);

    set_ui(100, "Relaunching MTF Recomp...");
    Sleep(300);

    char game_exe[MAX_PATH];
    snprintf(game_exe, sizeof game_exe, "%s\\mtf-recomp.exe", g_ctx.dest);
    if (file_exists(game_exe)) {
        STARTUPINFOA si = {0};
        PROCESS_INFORMATION pi = {0};
        si.cb = sizeof si;
        if (CreateProcessA(game_exe, NULL, NULL, NULL, FALSE, 0, NULL, g_ctx.dest, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    g_ctx.exit_code = 0;
    if (g_log) fclose(g_log);
    PostMessageA(g_ctx.hwnd, WM_UPD_DONE, 0, 0);
    return 0;
}

static void load_patch_notes(void) {
    if (!g_ctx.notes_path[0] || !file_exists(g_ctx.notes_path)) {
        SetWindowTextW(g_ctx.edit_notes, L"Downloading and installing the latest MTF Recomp update...");
        return;
    }
    FILE *f = fopen(g_ctx.notes_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 65536) {
        char *buf = (char *)malloc((size_t)sz + 1);
        if (buf) {
            size_t n = fread(buf, 1, (size_t)sz, f);
            buf[n] = 0;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
            if (wlen > 0) {
                wchar_t *wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
                if (wbuf) {
                    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wlen);
                    SetWindowTextW(g_ctx.edit_notes, wbuf);
                    free(wbuf);
                }
            }
            free(buf);
        }
    }
    fclose(f);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_UPD_PROGRESS: {
        int pct = (int)wp;
        char *text = (char *)lp;
        SendMessageA(g_ctx.prog, PBM_SETPOS, (WPARAM)(pct * 10), 0);
        SetWindowTextA(g_ctx.lbl_status, text ? text : "");
        char pbuf[16];
        snprintf(pbuf, sizeof pbuf, "%d%%", pct);
        SetWindowTextA(g_ctx.lbl_pct, pbuf);
        free(text);
        return 0;
    }
    case WM_UPD_DONE:
        if (g_ctx.exit_code != 0) {
            char full[768];
            snprintf(full, sizeof full,
                     "%s\n\nYour saves (profile/), settings (mtf.ini), and game files were NOT modified.",
                     g_ctx.err_msg);
            MessageBoxA(hwnd, full, "MTF Recomp Updater", MB_OK | MB_ICONERROR);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(g_ctx.exit_code);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void parse_cmdline(void) {
    int argc = __argc;
    char **argv = __argv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc) g_ctx.pid = (DWORD)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dest") && i + 1 < argc) snprintf(g_ctx.dest, sizeof g_ctx.dest, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--zip") && i + 1 < argc) snprintf(g_ctx.zip_path, sizeof g_ctx.zip_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--notes") && i + 1 < argc) snprintf(g_ctx.notes_path, sizeof g_ctx.notes_path, "%s", argv[++i]);
    }
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;
    (void)show;
    parse_cmdline();

    if (!g_ctx.dest[0] || !g_ctx.zip_path[0] || !file_exists(g_ctx.zip_path)) {
        MessageBoxA(NULL, "Missing --dest or --zip argument, or update.zip not found.",
                    "MTF Recomp Updater", MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "MtfUpdaterWnd";
    RegisterClassA(&wc);

    int w = 500, h = 360;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_ctx.hwnd = CreateWindowExA(WS_EX_APPWINDOW, "MtfUpdaterWnd", "MTF Recomp Updater",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (sx - w) / 2, (sy - h) / 2, w, h, NULL, NULL, hinst, NULL);

    HFONT font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_ctx.edit_notes = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                       18, 16, 448, 224, g_ctx.hwnd, NULL, hinst, NULL);
    g_ctx.lbl_status = CreateWindowA("STATIC", "Starting update...", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     18, 252, 386, 20, g_ctx.hwnd, NULL, hinst, NULL);
    g_ctx.lbl_pct = CreateWindowA("STATIC", "0%", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                  410, 252, 56, 20, g_ctx.hwnd, NULL, hinst, NULL);
    g_ctx.prog = CreateWindowExA(0, PROGRESS_CLASSA, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                 18, 278, 448, 24, g_ctx.hwnd, NULL, hinst, NULL);
    SendMessageA(g_ctx.prog, PBM_SETRANGE32, 0, 1000);
    SendMessageA(g_ctx.edit_notes, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_ctx.lbl_status, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_ctx.lbl_pct, WM_SETFONT, (WPARAM)font, TRUE);

    load_patch_notes();

    ShowWindow(g_ctx.hwnd, SW_SHOW);
    UpdateWindow(g_ctx.hwnd);

    HANDLE th = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}
#else
int main(void) { return 0; }
#endif
