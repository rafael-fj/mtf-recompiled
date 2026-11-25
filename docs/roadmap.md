# MTF Recomp — Implementation Roadmap (`F0`–`F8`)

Target: *Mario Tennis Fever* (Nintendo Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`).

## F0 — Foundation, Content Guard & Dump Validation (Complete)

- [x] Initialize repository structure, `.gitignore`, `.gitattributes`, `LICENSE` (MIT), `AGENTS.md`, `THIRD_PARTY_NOTICES.md`.
- [x] Implement `tools/check_content.py`, `.githooks/pre-commit`, and `.github/workflows/content-guard.yml`.
- [x] Create `config.example.toml` and local `config.local.toml` pointing read-only to the user's dump.
- [x] Port `tools/vendor/nup` and `profiles/0400D4201C6EA000/` (`profile.json`, `shader_facts.tsv`).
- [x] **Gate F0:** `python tools/check_dump.py` validates `main`, `sdk`, `main.npdm`, `nrr.nrr` (NRR v1), and `romfs/42` (`CRA.`), and `python tools/check_content.py` passes with 0 violations.

## F1 — Toolchain Setup & ARM64 Lifter (`main` + `sdk`) (Complete)

- [x] Install/configure build toolchain (`LLVM-MinGW 20261104-ucrt-x86_64` Clang 23.1.5 + `Ninja 1.13.2` in `toolchain/`, Python `lz4`/`zstandard`/`unicorn`).
- [x] Port `recomp/` (`nso.py`, `module.py`, `lifter.py`, `simd_c.py`, `jumptables.py`, `gen.py`, `layout.py`, `patches.py`) from `totk-pc-recomp` / `LASRecomp`.
- [x] Support Switch 2 / ARMv8.3 instructions (`PAC` hints/branches, `LDAPR*`, `LSE` atomics, `CRC32X`/`CRC32CX`) and `DT_RELR` relocations.
- [x] **Gate F1:** 100% of `main` (1,589 fns) and `sdk` (47,907 fns) lift to C with 0 unsupported instructions, pass Unicorn differential tests (40,000 encodings, 119,955 cases, 0 failures), compile, and link (`mtf-stage1.exe`).

## F2 — Horizon OS HLE, Boot Stub (`nxboot`) & `nro0` Capture (24.7 MB) (Complete)

- [x] Implement guest memory layout (`runtime/memory.c`: 64 GiB alias, 9 GiB pool), Switch 2 `svcGetInfo`, `31.25 MHz` timer (`rt_ticks`), 6-core threads (`NCORE = 6`), mutexes, condvars, and IPC (`runtime/kernel.c`, `runtime/svc.c`, `runtime/ipc.c`).
- [x] Implement `runtime/ro.c` (NRR v1 + `MTF_NRO_CAPTURE`) to capture the 24.7 MB `nro0` unpacked by `main`'s 6 threads, verify its SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`), index `main` + `sdk` + `nro0` in `tools/analisis/bindb.py`, and lift/compile `nro0` (60,968 fns, 151 chunks).
- [x] Implement directory-backed `rom:/` VFS mounting (`runtime/romfs.c`), `nn::fs` host storage (`runtime/fs_host.c`), `R_AARCH64_TLSDESC` relocation support (`runtime/loader.c`), and Stage-2 subsystem stubs (`runtime/stage2_stubs.c`).
- [x] **Gate F2:** `mtf-stage2.exe` boots `main` + `sdk` + `nro0` (with `TOTK_ASYNC_PREEMPT=0`), mounts `rom:/` and `save:/`, completes `nro0 DT_INIT` (197 entries), and reaches the first NVN2 call (`nvnDeviceGetProcAddress` at `nro0+0xaf71e4`).

## F3 — NVN2 Null Renderer, Audio (REV15) & VP9 Movies (`nn::codec`) (Complete)

- [x] Implement NVN2 proc table (~700 entry points, `nvnQueueWaitSync`, `nvnCommandBufferSetCopyImageStride`) with `MTF_GPU=null` fence/event pacing (`frame_rate.c`).
- [x] Port `audren.c` (REV15) + SDL3 audio output (`audio_mixer.c`, `audio_effects.c`, `audio_reverb.c`) and input (`input.c`, `input_win.c`, `ov_input.c`, `motion_fusion.c`, `GetNpadState`, `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, `SetGyroscopeZeroDriftMode`).
- [x] Port async `ov_codec.c` (`libvpx` VP9 2560x1440 decoder) + `ov_movie.c`, plus `nn::friends` offline stubs, `nn::oe` display resolution/notification event overrides, and system TTF fallback for missing shared fonts.
- [x] **Gate F3:** `mtf-stage2.exe --boot` with `MTF_GPU=null` and `TOTK_ASYNC_PREEMPT=0` runs at 60 Hz (`frame rate: limit 60 fps`, `present 1`), opens REV15 `audren` + SDL3 audio (`48000 Hz x240, voices 96`), and initializes `2560x1440` `libvpx` VP9 video decoder cleanly.

## F4 — `CRA.` Extraction & Offline Shader Translation (35,045 Shaders) [Complete]

- [x] Implement `tools/shaders/cra.py` (+ `cra_fast.c`), `extract.py`, `translate.py`, and `tools/xlate/` (Ryujinx Ampere `sm86` front-end + `GenerateCall` SPIR-V fix).
- [x] Handle passthrough geometry shader lowering (`SPV_NV_geometry_shader_passthrough` / `8x8x64` atmosphere LUT) and NVN2 compute `c[0]`/`c[1]` workgroup indexing (`shader_facts.tsv`).
- [x] **Gate F4:** All 35,045 shaders from `rom:/42` and `nro0` extract and translate to SPIR-V without fatal errors (`35045/35045` in `53.6s`, `0 failed`).

## F5 — Vulkan 1.3 Backend & Playable 60 FPS

- [x] Implement Switch 2 texture GOB deswizzling (`tex_deswizzle.c`), ASTC (`tex_astc.c`, `vk_astc.c`), `nvnWindowSetCrop`, and silent ignore of null vertex streams `16..31` (`nvn_api_cmd.c`).
- [x] Implement Vulkan 1.3 renderer, recorder thread (`vk_rec.c`), offline NVN2 SPIR-V loader (`gfx_shader.c`), and SPIR-V `OpVariable` / `GenerateCall` / unused `c0` binding fixes (`Module.cs`, `Instructions.cs`, `gfx_shader.c`).
- [x] **Gate F5:** Opening movie, menus, Adventure intro, and Free Play singles on Stadium Court render at 60 FPS (verified in-game at 60–144 FPS) with 0 Vulkan validation errors (`MTF_VK_VALIDATION=1` with Vulkan SDK `1.4.363.0`: 0 errors, 0 warnings).

## F6 — PC Features (4K, DLSS/DLAA/FSR1, HDR10, Gyro, PT-BR/EN Menu)

- [x] Port FSR 1 (`vk_fsr.c`), SMAA (`vk_smaa.c`), HDR10 (`vk_hdr.c`), uniform snapshot (`vk_usnap.c`), and `perfmon.c`.
- [x] Port ImGui two-pane settings menu (`runtime/ui/`, `runtime/third_party/imgui/`, `F1`/`Esc`/`Minus+Plus`) in English and Brazilian Portuguese (`pt-BR`) + `mtf.ini` persistence.
- [x] **Gate F6:** PC enhancement options (`TOTK_UPSCALER=fsr`, `TOTK_SMAA=high`, `TOTK_MENU_LANG=pt-BR`, `TOTK_PERF_OVERLAY=full`) toggle live, render across all 7 menu tabs (`menu-test-35-0.png`..`65-6.png`), and persist/reload via `mtf.ini`. (Optional NVIDIA NGX DLSS/DLAA skipped for MIT license compliance.)

## F7 — Diagnostics, Frame Replay & Visual Bugfixes

- [x] Port flight recorder & thread snapshots (`runtime/flight.c`, `flight.h`, `tools/flight_view.py`), RenderDoc integration (`runtime/rdoc.c`), and VRAM headroom tracker (`runtime/vk_headroom.c`).
- [x] Close-up grass blade rendering (`TESS`, `hs`/`ds` tessellation stages) and non-NVIDIA shadow/grass portability (`DEPTH_TWIN` in `runtime/vk_exec_draw.c`, AMD omap/image fixes in `runtime/vk_pipeline.c` and `runtime/vk_texture.c`) implemented.

## F8 — End-User Installer (`MTF-Setup.exe`) & Release Packaging

- [x] Implement Tkinter local-build installer & launcher ([installer/mtf_setup.py](installer/mtf_setup.py)) + clean release packager ([tools/make_release.py](tools/make_release.py)).
- [x] **Gate F8:** `python tools/make_release.py` produces a clean `2.79 MiB` release archive (`build/release/mtf-recomp-0.1.0-dev-src.zip`) with **0 content-guard violations** and zero game files.

## F9 — Fork 1:1 (`0.1.0+6caba99`) Feature & Optimization Roadmap

Base: clean 1:1 fork (`build/fork/bin/mtf-recomp.exe` via `tools/build_fork.py`).

> **Pipeline Rule:** Before tackling any issue below, consult [docs/switch2-recomp-rag.md](switch2-recomp-rag.md) to reuse proven solutions (copyable MIT code or clean-room reference for GPL-3.0).

1. **High FPS (> 60 FPS), Frame Pacing & Frame Generation**
   - [x] Expose `MTF_FPS_LIMIT` (`30|40|45|60|90|120|144|240|display|unlimited`) in `cfg.c`, in-game menu (`F1`), and `nro0` pacer (`0x70035611b8`): measured **144 FPS** (`display`) and **~1925 FPS** (`unlimited` on opening) — useful for optimization benchmarks.
   - [x] **FPS > 60 at `1x` real speed via Frame Generation (`MTF_FRAMEGEN`):** ported from `mkw-recomp` / `SkyboundTotk` (MIT, `vk_framegen.c` + `fg_*_spv.h`, `off|120|monitor|unlimited` modes with 5-image swapchain in `gfx_present.c` for up to `4x` = `240 FPS`). Validated in-game (`60 FPS` base + `2x` = smooth `120 FPS` at `1x` speed).
   - [ ] *(Optional / future)* Decouple native simulation from `60 Hz` (`frame_rate.c` + delta-time patches): match testing with `MTF_FPS_LIMIT=144` without Frame Gen confirmed ~2.4x speedup due to the fixed `1/60s` step.
2. **Render Resolution, Upscaling & Camera/Ultrawide**
   - [x] **Clear Render Resolution + Upscaling + >4K/Supersampling flow:** `OUTPUT_RES=auto` tracks the window/display (`1080p` in a `1080p` window, no forced `1440p` downscale), `ov_resolution.c` supports up to `5K (2880p)` / `8K (4320p)`, and `RENDER_SCALE` spans `50%` to `200%` (supersampling); `F1` menu reorganized in order: Render scale → Output resolution → Upscaler (`FSR 1`/`Bilinear`) → `DLAA/DLSS`.
   - [ ] *(Optional / future)* Evaluate `FSR 3.1` / `NIS` from RAG (`xc2-ns2-recomp`, `Recompia`) and implement `ULTRAWIDE` (`21:9`/`32:9`), `HUD_POSITION`, and `FOV`.
   - [ ] *(Experimental)* `ULTRAWIDE` (`21:9`/`32:9`), `HUD_POSITION` (`edges|centered`), and `ASPECT_PATCH` (`nro0` 3D court expands, but 2D menus stretch, match HUD stays in 16:9 bounds, and announcer 3D portrait is hidden — needs `nro0` UI/viewport patches).
   - [x] Implement `FOV` (`res_fov_live()`), `WINDOW_MODE` (`windowed|borderless|fullscreen`), and `HDR` luminance/shadow-lift tuning.
3. **Anti-Aliasing, Upscaling & Native HDR**
   - [x] Port `SMAA` (`vk_smaa.c` + `smaa_*_spv.h`) and `SHADOW_SCALE` in `F1 -> Screen`.
   - [ ] *(Deferred / low ROI)* `HDR_NATIVE` + `spirv_hdr_shoulder.c`: unlike DKB, MTF has no native HDR10 pipeline (tonemaps directly to 8-bit sRGB); `hdr_compose.comp` already reconstructs HDR from pre-tonemap `light` + `bloom` + `exposure`, whereas promoting post-tonemap targets to `RGBA16F` across ~22 shaders adds high complexity for minimal gain.
   - [ ] Expose `SHADOW_RES` in the in-game menu (`metas[]` in `runtime/cfg.c`).
4. **CPU, GPU & Memory Optimizations (Vulkan Backend — Tier 2)**
   A/B measurement methodology and evolution table in [docs/perf-methodology.md](perf-methodology.md).
   - **Stage 2.1 — Memory, VRAM & Stutter Reduction (16 GB RAM / 8 GB VRAM PCs):**
     - [x] Port `vk_headroom.c` (`MTF_VRAM_HEADROOM`, background VMA block pre-allocation with automatic backoff above 85% VRAM budget).
     - [x] Port `MTF_HEAP_LAZY` (`runtime/platform/win32.c` `fault_veh` + `runtime/memory.c` `mem_touch`) and `MTF_SPARSE_MIRROR_COPY` (`runtime/vk_memory.c` + `mirror_sparse_spv.h`).
     - [x] Implement Windows commit headroom control (`MTF_COMMIT_HEADROOM_MIB`, default `512` MiB, clean-room via `GlobalMemoryStatusEx` `ullAvailPageFile` in `plat_mem_avail_commit`) to prevent OOM on 16 GB RAM.
     - [x] **Gate 2.1:** `check_content.py` clean, zero build errors, and boot smoke test verifying `MTF_VRAM_HEADROOM`, `MTF_HEAP_LAZY`, `MTF_SPARSE_MIRROR_COPY=auto`, and `MTF_COMMIT_HEADROOM_MIB` (`5076 MB` initial boot); match measurements recorded in [docs/perf-methodology.md](perf-methodology.md).
   - **Stage 2.2 — Vulkan Replay CPU Performance:**
     - [x] Port multi-threaded parallel pass replay (`MTF_REPLAY_THREADS`, default `6` on >=12 CPUs / `4` on >=8 CPUs / `0` on AMD) in [runtime/vk_exec.c](runtime/vk_exec.c) + per-thread `DrawCtx` in [runtime/vk_exec_draw.c](runtime/vk_exec_draw.c) + inherited query support in [runtime/vk_exec_xfer.c](runtime/vk_exec_xfer.c) + per-thread local write-back in [runtime/vk_memory.c](runtime/vk_memory.c).
     - [x] Evaluate `vk_spec.c` + `vk_exec_draw_helper.c` (`MTF_REPLAY_PAR`): kept optional/future since `MTF_REPLAY_THREADS=6` already parallelizes large passes (>=256 draws) without duplicating `vk_exec_draw.c`.
     - [x] **Gate 2.2:** Clean build, zero `check_content.py` violations, and smoke test confirming `MTF_REPLAY_THREADS=6`; A/B measurements (`MTF_REPLAY_THREADS=0` vs `6`) recorded in [docs/perf-methodology.md](perf-methodology.md).
   - **Stage 2.3 — Shader Robustness & Documentation:**
     - [x] Port `spirv_nan_guard.c` (optional `MTF_NAN_GUARD` for fragment shaders: `0|zero|mark`).
     - [x] Record `dkb-pc-recomp` (`4e7f9b2`) MIT credits in `THIRD_PARTY_NOTICES.md`.
5. **Settings Menu (`F1`) & Control Remapping**
   - [x] Port full keyboard (2 slots + `SHAKE` + analog sticks) and gamepad remapping (`binds_page.cpp` + `input_map.{c,h}` + `input_capture` + `MTF_BIND_*`) in `F1 -> Controls` (**Controller** and **Keyboard** sub-tabs, instant `mtf.ini` save and live application).
   - [x] Add Tier 2 options (`MTF_REPLAY_THREADS`, `MTF_VRAM_HEADROOM`, `MTF_HEAP_LAZY`, `MTF_SPARSE_MIRROR_COPY`, `MTF_NAN_GUARD`) to `F1 -> Advanced`, plus `F1 -> Cheats` (`runtime/cheats.{h,c}`) and in-game GitHub release notifier (`runtime/update_check.{h,c}`).
6. **Online Multiplayer (`F1 -> Multiplayer`)**
   Full roadmap and detailed backlog in [docs/multiplayer-roadmap.md](multiplayer-roadmap.md).
   - [x] Multiplayer input selection (`P1..P4`) in `F1` (`runtime/input.c`, `runtime/ov_hid.c`), `ProfanityFilter` crash fix, and split-screen DLAA/DLSS fix (`vk_dlaa.c`).
   - [x] **Phase 1 & 2:** Embedded Browser Streaming host (`runtime/stream_host.c` + `runtime/stream_webrtc.cpp`: VP8 + Opus + WebRTC UDP DataChannel / Cloudflare Tunnel / Local Host + remote P2..P4 inputs).
   - [x] **Phase 3:** Ordered partial-reliability media channel, automatic WebSocket fallback, `Performance`/`Balanced`/`Quality` presets, and 3s player connect/disconnect toasts.
7. **Diagnostics, Mods, Distribution & Build Pipeline**
   - [x] Add RecompLauncher SDK support (`recomplauncher.json` Levels 1–3, headless `--prepare`, `RECOMPLAUNCHER_*` env integration) and GitHub Actions `--onedir` release workflow (`.github/workflows/release.yml`).
   - [ ] Port `flight.c` (`FLIGHT`), `vk_crumbs.c`, `prof_win.c`/`perf.c`, `rdoc.c`, `mods_scan.h` (`ROMFS_MODS`/`ROMFS_PATCHES`), `MOUSE_CAMERA`, and `RUMBLE_TRIGGERS`.
   - [ ] Keep `tools/build_fork.py` + `tools/mtf_launcher.py` aligned with clean fork builds.
8. **Optimization Sprint (`v0.1.8`, branch `test`)** — plan in [docs/optimization-sprint.md](optimization-sprint.md)
   - [x] T0 Baseline capture (NVIDIA + AMD iGPU) in [docs/perf-methodology.md](perf-methodology.md).
   - [x] T1 Frame-time telemetry (1% / 0.1% lows, pipeline waits) written to `mtf.log` (`MTF_PERF_LOG=1`).
   - [x] T2 Validate and decouple NVIDIA slice-parallel recording from async GPL (`MTF_REPLAY_THREADS=0` decoupled + async GPL verified over 4,800 frames).
   - [x] T3 Non-blocking pipeline prewarm (`MTF_PIPE_PREWARM_BLOCK_MS=3000`: AMD iGPU boot reduced from `21.99 s` to `4.04 s`).
   - [x] T4 Pipeline spike reduction (`MTF_ONESHOT_BUDGET_MS=16`, `MAX_WORKERS=8`, `MTF_GPL_DRAW_LINK=0` default on AMD).
   - [x] T5a Dynamic resolution spike (`ov_resolution_apply` sizes targets once at boot; arbitrary live `RENDER_SCALE` deferred).
   - [ ] T6 `MTF_REPLAY_ASYNC` A/B experiment.
   - [ ] T7 Docs, merge `test` to `main`, `v0.1.8` notes.
