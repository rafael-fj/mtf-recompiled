# MTF Recomp — Linux & Steam Deck Port Action Plan (`v0.2.0`)

Target platforms: **Desktop Linux (`x86_64`, X11/Wayland)** and **SteamOS / Steam Deck** (GameScope + Deck controls).

> [!NOTE]
> The runtime already separates OS and window-system boundaries via [runtime/platform/platform.h](../runtime/platform/platform.h) (`win32.c` vs `linux.c`) and [runtime/gfx_wsys.h](../runtime/gfx_wsys.h) (`gfx_window_win.c` vs `gfx_window_sdl.c`), which keeps the port clean and modular.

---

## Phase L0 — Build System & Dependency Tooling (`CMake` + `deps`)
- **Goal:** Compile `mtf-recomp` on Linux `x86_64` (Clang / GCC + LLD) alongside Windows.
- **Tasks:**
  1. Update [CMakeLists.txt](../CMakeLists.txt) with `if(WIN32) ... else() ... endif()`:
     - Remove `RC` language (`runtime/mtf.rc`) and Win32 link libraries (`user32`, `gdi32`, `mfplat`, `winhttp`, `ws2_32`, `pdh`, etc.) on non-Windows.
     - Swap `runtime/platform/win32.c`, `runtime/gfx_window_win.c`, `runtime/ui/menu_win.cpp` for Linux implementations (`linux.c`, `gfx_window_sdl.c`, `menu_sdl.cpp`).
     - Link `-lpthread -ldl -lm` + Linux libs (`vulkan`, `SDL3`, `opus`, `vpx`, `datachannel`, `juice`, `srtp2`, `usrsctp`, `mbedtls`, `libcurl` if enabled).
     - Replace `-Wl,--stack,16777216` with Linux linker equivalent or main-thread `setrlimit(RLIMIT_STACK, ...)` / worker bootstrap.
  2. Add Linux dependency discovery/build script (`tools/make_deps_linux.py` or system/vcpkg/AppImage bundle for SDL3, libvpx, opus, libdatachannel, VMA).
  3. Set up GitHub Actions `ubuntu-latest` CI build check (`check_content.py` + `cmake` runtime compile).

## Phase L1 — POSIX Platform Layer (`runtime/platform/linux.c` & `memory.c`)
- **Goal:** Implement all primitives in [runtime/platform/platform.h](../runtime/platform/platform.h) and guest memory/fault handling on Linux.
- **Tasks:**
  1. **Implement `runtime/platform/linux.c`:**
     - Threads (`pthread_create`, `pthread_setname_np`, `PLAT_STACK_DEFAULT` 16 MiB, `sched_getaffinity` / `sysconf(_SC_NPROCESSORS_ONLN)`).
     - Synchronization (`pthread_mutex_t`, `pthread_cond_t`, `clock_gettime(CLOCK_MONOTONIC)` / `clock_nanosleep`).
     - **Fibers (`PlatFiber`):** `ucontext` or minimal x86-64 SysV ABI assembly context switch (callee-saved `rbx, rbp, r12..r15`, `mxcsr`, `x87 fcw`, `rsp`), matching `win32.c`.
     - **Async preemption (`PlatIntr`):** `pthread_kill` + `SIGUSR1` handler reading/redirecting `ucontext_t` `REG_RIP` / `REG_RSP` (with `SA_SIGINFO`).
     - Priority & power hints (graceful no-op when `CAP_SYS_NICE` / `RLIMIT_NICE` is absent, as already documented in `platform.h`).
  2. **Virtual Memory & Guest Faults (`runtime/memory.c`, `runtime/vk_memory.c`):**
     - Replace `VirtualAlloc` / `VirtualProtect` / `AddVectoredExceptionHandler` with `mmap` / `mprotect` + `sigaction(SIGSEGV)` handler for guest write-watch/layout checks.
  3. **Filesystem & Misc (`runtime/fs_host.c`, `runtime/hostfn.c`, `runtime/kernel.c`):**
     - Guard remaining `_WIN32` paths (`MAX_PATH` -> `PATH_MAX`, `/` separators, `dlopen`/`dlsym`, `exe` path via `/proc/self/exe`).

## Phase L2 — SDL3 Window, Vulkan Surface, Input & ImGui (`gfx_window_sdl.c`)
- **Goal:** Open the game window, render Vulkan 1.3 + Dear ImGui `F1` overlay, and accept keyboard/mouse/gamepad input on X11, Wayland, and Steam Deck GameScope.
- **Tasks:**
  1. **Implement `runtime/gfx_window_sdl.c`** fulfilling [runtime/gfx_wsys.h](../runtime/gfx_wsys.h):
     - `SDL_CreateWindow(..., SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE)` + `SDL_Vulkan_CreateSurface()`.
     - Handle resize/minimize/focus events, display refresh rate (`SDL_GetCurrentDisplayMode`), borderless/fullscreen toggling (critical for **Steam Deck GameScope**, which runs games in a 1280×800 borderless/fullscreen compositor).
  2. **Implement `runtime/ui/menu_sdl.cpp`** (replacing `menu_win.cpp`) and update `runtime/input.c` / `runtime/input_win.h`:
     - Route SDL3 keyboard/mouse events to Dear ImGui (`F1` / `Esc` / `Minus+Plus`) and SDL3 Gamepad API (Steam Input / Deck built-in controller works out of the box via SDL3).
  3. **Graphics & Telemetry Guards:**
     - `runtime/vk_dlaa.c`: gate `nvngx_dlss.dll` / NGX probing cleanly on Linux (skip or use `libnvidia-ngx.so` if ever supported; Steam Deck is RDNA 2 AMD).
     - `runtime/perfmon.c`: replace Windows `pdh` counters with `/proc/stat` and `/proc/self/statm`.

## Phase L3 — Networking, Updater & Multiplayer Streaming
- **Goal:** Feature parity for `ldn`, GitHub update checker, and Browser Remote Play.
- **Tasks:**
  1. **`runtime/ov_net.c` & `runtime/ldn.c`:** Replace Winsock2 (`ws2_32` / `iphlpapi`) with POSIX sockets (`sys/socket.h`, `getifaddrs`, `poll`).
  2. **`runtime/update_check.c`:** Replace `WinHTTP` with `libcurl` (or `curl` subprocess / disable gracefully if unavailable).
  3. **`runtime/stream_hwenc.cpp` & `runtime/stream_host.c`:**
     - Replace Windows Media Foundation (`mfplat`) with a clean Linux encoder path (VA-API / NVENC or `libvpx` VP8/VP9 software fallback, which already builds cross-platform).

## Phase L4 — Steam Deck Optimization & Packaging (`v0.2.0`)
- **Goal:** Smooth out-of-the-box experience on Steam Deck (1280×800, RDNA 2 iGPU) and Linux desktops.
- **Tasks:**
  1. **Steam Deck Defaults:**
     - Test RDNA 2 shader behavior (including the `GeometryShaderPassthroughNV` lowering and ball-trail rendering on Mesa `radv`).
     - Validate `MTF_DYNAMIC_FPS=1` + `low` / `Recommended` preset at 1280×800 on Deck (`45–60 FPS` without slow motion).
     - Add `800p` (`1280x800`) / `16:10` awareness if needed in `ov_resolution.c` / `menu.cpp`.
  2. **Packaging & Installer:**
     - Update `installer/` (`cli.py`, `gui.py`, `steps.py`) and `tools/make_release.py` to support Linux (`MTF-Recomp-v<ver>-Linux-x64.tar.gz` / AppImage + `recomplauncher.json` Linux entry).
     - Add Linux release job to `.github/workflows/release.yml` (after local validation).

---

## Suggested Execution Order (Milestones)
1. **Milestone 1 (Headless Boot on WSL2/Ubuntu):** `L0` + `L1` -> compile & boot with `MTF_GPU=null` to gameplay tick.
2. **Milestone 2 (Vulkan + SDL3 Window):** `L2` -> boot to menu & match on desktop Linux with controller input.
3. **Milestone 3 (Full Feature Parity):** `L3` -> POSIX networking + streaming fallback.
4. **Milestone 4 (Steam Deck Validation & Release):** `L4` -> test on SteamOS / Deck, then package `v0.2.0-linux` pre-release for user validation before publishing.
