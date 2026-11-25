# MTF Recomp — Development Log

## 2026-11-15 — Phase F0: Repository Setup, Reference Forensics & Dump Validation

- **Goal:** Establish a clean, legally compliant repository (`https://github.com/rafael-fj/mtf-recompiled.git`) and verify the user's local *Mario Tennis Fever* (`0400D4201C6EA000`, v1.1.1) dump without modifying any game files.
- **Findings from reference forensics:**
  - Recovered the exact component lineage of the archived `kayochiaradia/mtf-recomp` via its `THIRD_PARTY_NOTICES.md` (`totk-pc-recomp` + `LASRecomp` + `nxnative` + `MacNativeGames` + `d2r-mobile` + `nup` + `SavageWindRecomp`, keeping the entire codebase MIT-compatible).
  - Confirmed via `seedds/nxnative` (`dm/games/0400D4201C6EA000/`) that `main` is a ~600 KB `nxboot` stub that unpacks a 24.7 MB `nro0` at runtime using 6 threads, and `romfs/42` (`CRA.`) holds 8,671 files and 35,045 shaders indexed inside `nro0`'s `.rodata`.
- **Experiment (`python tools/check_dump.py`):**
  - Initial check flagged an MD5 mismatch because `nxnative/game.tsv` records MD5s of uncompressed/ELF-stage modules (`104bce68...`, `32a9ac14...`), whereas the compressed `NSO0` files on disk in `updates/327680/exefs/` have MD5s `115e98ed471509dd30785b35cf8a0d58` (`main`, 14,069,153 B) and `17fd7a3ffd8eb8af7500310841d4cf54` (`sdk`, 7,326,594 B).
  - Parsed `updates/327680/romfs/.nrr/nrr.nrr`: confirmed Switch 2 **NRR v1** (`byte[5] == 1`), `program_id = 0400D4201C6EA000`, and 1 registered NRO SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`).
  - Confirmed `updates/327680/romfs/42` has magic `b'CRA.'` and size `8,172,385,024` B.
- **Outcome:** `tools/check_dump.py` and `tools/check_content.py` both exit `0`. Phase F0 complete.
- **Next:** Install Windows build toolchain (`MSYS2 UCRT64 Clang` / `LLVM-MinGW`, `CMake`, `Ninja`, `.NET SDK`) and begin Phase F1 (ARM64 lifter).

## 2026-11-15 — Phase F1: Toolchain Setup, ARM64 Lifter, Differential Tests & Stage-1 Link

- **Goal:** Port the MIT ARM64 $\rightarrow$ C lifter (`recomp/`), verify instruction semantics against Unicorn, and compile + link all of `main` and `sdk` into `mtf-stage1.exe`.
- **Toolchain provisioned (under git-ignored `toolchain/`):**
  - `LLVM-MinGW 20261104-ucrt-x86_64` (`clang 23.1.5` + `lld`), `Ninja 1.13.2`, Python packages `lz4`, `zstandard`, `unicorn`.
- **Experiments & Measurements:**
  - `python recomp/gen.py --exefs <dump>/updates/327680/exefs --out generated/v327680 --modules main,sdk --jobs 8`:
    - `main`: 1,589 functions, 4 chunks, 10 MB of C, 124 jump tables, 0 all-label fallbacks, 292 `br` via dispatch, **0 unsupported instructions**.
    - `sdk`: 47,907 functions, 50 chunks, 138 MB of C, 669 jump tables, 27 all-label fallbacks, 11,922 `br` via dispatch, **0 unsupported instructions**. Total lift time: **12s**.
  - `python tests/diff/lift.py --exefs <dump>/updates/327680/exefs --modules main,sdk --jobs 8`:
    - 40,000 distinct encodings, **119,955 cases executed against Unicorn: 0 failing cases, 0 failing encodings** (77s).
  - `python tools/build_stage1.py --jobs 8`:
    - Compiled all 54 generated C chunks + tables + `runtime/fma.c` + minimal runtime stub and linked `build/v327680_stage1/mtf-stage1.exe` cleanly.
    - Executed `mtf-stage1.exe`: `MTF Stage-1 link check OK: main=1589 fns, sdk=47907 fns`.
- **Outcome:** Gate F1 passed. `python tools/check_content.py` passed with 0 violations.
- **Next:** Begin Phase F2 — port Horizon OS HLE (`memory.c`, `kernel.c`, `svc.c`, `ipc.c`, `ro.c`) to boot `main`, capture the 24.7 MB `nro0` unpacked by `nxboot`'s 6 threads, and lift/compile `nro0`.

## 2026-11-15 — Phase F2: Horizon OS HLE, `nro0` Capture, Lifting & First NVN2 Call

- **Goal:** Build Stage-2 Horizon OS HLE executable (`mtf-stage2.exe`), boot `main` (`nxboot`), capture the 24.7 MB `nro0`, lift & compile `nro0`, index all modules in `build/bindb.sqlite`, and reach the first NVN2 call.
- **Implementation:**
  - Ported MIT Horizon OS HLE runtime (`kernel.c`, `memory.c`, `svc.c`, `ipc.c`, `dispatch.c`, `loader.c`, `boot.c`, `fs_host.c`, `ov_system.c`, `ov_gfx.c`, `ov_input.c`, `gfx_null.c`, `excl.c`, `platform/*`) from `dkb-pc-recomp`.
  - Implemented clean-room `runtime/ro.c` (Switch 2 NRR v1 `+0x4c0`, `MTF_NRO_CAPTURE`) and directory-backed `runtime/romfs.c` (serving `updates/327680/romfs/` directly without duplicating the 8.17 GiB `42` archive).
  - Updated `kernel.c` `CreateThread` (`svc #0x08`) to allow Switch 2's 6 application cores (`0..NCORE-1`, `NCORE=6`), `fs_host.c` to override `nn::fs::SetAllocator` / `IsEnabledAccessLog`, and `loader.c` to load `NRO0` and resolve `R_AARCH64_TLSDESC` (`1031`).
  - Extended `recomp/nso.py`, `recomp/layout.py`, and `recomp/gen.py` to support `NRO0` (`--nro name=path`) and ported `tools/analisis/bindb.py` from `mkw-recomp`.
- **Experiments & Measurements:**
  - `MTF_GPU=null MTF_NRO_CAPTURE=build/nro build/v327680_stage2/mtf-stage2.exe --boot`: captured `build/nro/1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975.nro` (`24,764,416` bytes, SHA-256 matches `nrr.nrr`).
  - `python recomp/gen.py ... --modules nro0 --nro nro0=build/nro/...nro`: lifted `nro0` (**60,968 functions**, 151 chunks, 336 MB C, 1,369 jump tables, 1 unsupported `ldst`, 54s).
  - `python tools/analisis/bindb.py`: indexed `main` + `sdk` + `nro0` into `build/bindb.sqlite` (`98,560` functions, `29,422` symbols, `573,750` calls, `54,969` strings, `77,561` fptrs).
  - `python tools/build_stage2.py --jobs 16`: compiled all 151 `nro0` chunks + `main` + `sdk` + HLE runtime into `mtf-stage2.exe` (68.9s) with zero unresolved strong imports.
  - `MTF_GPU=null TOTK_ASYNC_PREEMPT=0 build/v327680_stage2/mtf-stage2.exe --boot`: booted through `nn::ro::LoadModule` -> `nro0 DT_INIT` (197 entries) -> `nn::account` -> `save:/` mount -> **reached `nvnDeviceGetProcAddress` at `nro0+0xaf71e4`**.
- **Outcome:** Gate F2 passed. `python tools/check_content.py` verified.
- **Next:** Begin Phase F3 — implement NVN2 proc table (~700 entry points) and `MTF_GPU=null` fence/event pacing, `audren` REV15 audio, and `ov_codec` VP9 movie decoding.

## 2026-11-15 — Phase F3: NVN2 Null Renderer, Audio (REV15), Input & VP9 Movies
- **NVN2 front-end (`runtime/nvn_*.c`, `runtime/gfx_c0.c`, `runtime/tex_deswizzle.c`):** Ported MIT NVN2 proc table and command recorder from `dkb-pc-recomp`. Added `nvnQueueWaitSync` (`nvn_api_core.c`) and `nvnCommandBufferSetCopyImageStride` / `nvnCommandBufferGetCopyImageStride` (`nvn_api_cmd.c`).
- **Dependencies (`toolchain/deps-mingw/`):** Provisioned `mingw-w64-ucrt-x86_64` `SDL3` (3.4.20), `libiconv` (1.19.1), `libvpx` (1.17.0), `opus` (1.6.1), `zstd` (1.5.7), `vulkan-headers`, `vulkan-loader` (1.4.363), and `vk_mem_alloc.h` (v3.2.1).
- **Audio, Input, Pacing & Codec:** Ported `audren.c` (REV15), `audio_mixer.c`, `audio_effects.c`, `audio_reverb.c`, `input.c`, `input_map.c`, `input_script.c`, `input_win.c`, `motion_fusion.c`, `frame_rate.c`, `perf.c`, `ov_codec.c`, `ov_movie.c` from `dkb-pc-recomp`.
- **HLE additions for MTF (`ov_input.c`, `ov_system.c`):** Added `nn::hid::GetNpadState` (singular), `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, `SetGyroscopeZeroDriftMode`, `GetSixAxisSensorState`, `GetSupportedNpadStyleSet`, `SetNpadJoyAssignmentModeSingle`, `SwapNpadAssignment`, `ShowControllerStrapGuide`; added `nn::friends` offline stubs; added `nn::oe::GetDefaultDisplayResolution`, `GetDefaultDisplayResolutionChangeEvent`, `GetNotificationMessageEvent`, `TryPopNotificationMessage`, `SendServerMaintenanceOverlayNotification`; added `C:/Windows/Fonts/arial.ttf` fallback in `ov_system.c` when Switch shared `.bfttf` fonts are absent.
- **Gate F3 verified:** `mtf-stage2.exe --boot` (`MTF_GPU=null`, `TOTK_ASYNC_PREEMPT=0`) reached `frame rate: limit 60 fps`, `present 1`, opened REV15 `audren` (`48000 Hz x240, mix buffers 30, voices 96, effects 14, sinks 2`) with SDL3 audio output, and initialized `2560x1440` `libvpx` VP9 decoder (`codec: VP9 decoder ... 2560x1440 on the host (libvpx)`).

## 2026-11-15 — Phase F4: `CRA.` Decryption, Shader Extraction & Ampere SPIR-V Translation
- **`rom:/42` (`CRA.`) decryption (`tools/shaders/cra.py`, `tools/shaders/cra_fast.c`):**
  - Solved Camelot `CRA.` table derivation from `nro0` `.rodata`: `_X10_XOR = 0x02052636 ^ 0xA756E6F0`, all 8 `x26 & 7` block-scrambling cases (`swap_in = (case & 2) == 0`, `odd_out = (case & 1) != 0`), `0x00` null-terminated name decoding, and extension table at `0x7003556d18` (direct file offsets).
  - Compiled `build/cra_fast.dll` with `clang -O3 -shared` for native decryption speed; verified `--pattern "ssg_system/*" --verify` (16/16 mod-16 entries, 0 errors) and `--pattern "shader/*" --verify` (8/8 mod-16 entries, 0 errors).
- **Shader extraction (`tools/shaders/extract.py`):**
  - Unpacked `MMMM` chunks, Zstd (`Z\0\0\0`), LZ4 (`0x94`), raw (`0x70`/`0x80`), zlib (`0x00`/`0x10`/`0x20`/`0x30`), `NARC`/`SARC`, and `BNSH`+`grsc` across 1,844 `rom:/42` entries + `nro0` in `20.8s`.
  - Extracted **35,045 unique shaders** (`6,915 vs`, `27,944 ps`, `78 cs`, `2 hs`, `2 ds`, `104 gs`) and **50,287 `vs`/`ps` PSO pairs** into `build/v327680/shaders-nvn2/`.
- **Ampere `sm86` SPIR-V translation (`tools/vendor/ryujinx/`, `tools/xlate/Program.cs`, `tools/shaders/translate.py`):**
  - Synced `seedds/nxnative` (MIT) `Ryujinx.Graphics.Shader` (`sm86`) and `dmsh_xlate/Program.cs` (`totk_xlate.exe` via `tools/build_xlate.py`).
  - Fixed `GenerateCall` (`CodeGen/Spirv/Instructions.cs` + `CodeGenContext.TryGetLocalPointer`) to materialize temporary `Function`-storage-class `OpVariable`s for non-`LocalVariable` call arguments emitted by `AstOptimizer`.
- **Gate F4 verified:** `python tools/shaders/translate.py` translated **35,045 / 35,045 shaders in 53.6s (654.7 shaders/s, 0 failed)** into `build/v327680/shaders-spv/`.

## 2026-11-15 — Phase F5: Vulkan 1.3 Backend, Offline NVN2 Shader Loader & 60 FPS Boot
- **Vulkan 1.3 + Window + ASTC + SPIR-V runtime (`runtime/vk_*.c`, `gfx_window*.c`, `tex_astc*.c`, `spirv_*.c`):** Ported from `dkb-pc-recomp` (MIT), integrated `clang++` (`-std=c++20`) + `-lvulkan-1 -ldwmapi -lc++` into `tools/build_stage2.py`, and pruned F5 stubs in `runtime/stage2_stubs.c`.
- **NVN2 shader loading (`runtime/vk_exec_draw.c`, `runtime/gfx_shader.c`):** Added NVN2 control header stage/length decoding (`c[0x56]`, `*(uint32_t*)(c + 0x44)`) and `nvn2_offline_get` to load pre-translated `<key16>.spv` + `<key16>.res.json` from `build/v327680/shaders-nvn2/spv/`.
- **Boot & SPIR-V fixes discovered on RTX 5070 Ti:**
  - `runtime/kernel.c` (`StartThread`): added brief startup yield (`plat_process_uptime_ns() < 2s`) to eliminate `nxboot` 6-thread stack-slot argument race.
  - `tools/vendor/ryujinx/src/Spv.Generator/Module.cs` (`AddLocalVariable`): placed function-scope `OpVariable`s in the entry block right after `OpLabel` instead of mid-function.
  - `tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Instructions.cs` (`GenerateCall`): materialized typed `Function`-scope `tempVar`s (with bitcast copy-in / copy-out) when passing a `LocalVariable` whose `VarType` differs from `function.GetArgumentType(i)` (fixed `nvgpucomp64.dll+0x1909f2` crash across 433 affected shaders). Re-translated all **35,045 / 35,045** shaders (`79.9s`, `0 failed`).
  - `runtime/gfx_shader.c` (`nvn2_offline_get`): backfilled unused `OpDecorate ... Binding >= 1` entries (`cb0`) into `GFX_RES_UBO` so `vk_pipeline.c` `layout_covers` builds all pipelines.
  - `runtime/nvn_api_cmd.c` (`bind_vertex_buffers`): silently ignored null clears on streams `16..31`.
- **Verification:** `mtf-stage2.exe --boot` (`MTF_GPU=vulkan`, `TOTK_ASYNC_PREEMPT=0`) initializes Vulkan 1.3 on RTX 5070 Ti, opens swapchain (`1920x1061`, mailbox, 60 FPS), compiles compute + graphics pipelines with 0 failures, runs VP9 `2560x1440` decoder, and presents `600+` frames cleanly. (Note: `VK_LAYER_KHRONOS_validation` is not installed on the test machine.)

## 2026-11-15 — Phase F6: PC Enhancements (FSR 1, SMAA, HDR10, PerfMon, PT-BR/EN ImGui Menu, `mtf.ini`)
- **Graphics & Perf (`runtime/vk_fsr.c`, `vk_smaa.c`, `vk_hdr.c`, `vk_usnap.c`, `perfmon.c`):** Ported MIT implementations + SPIR-V headers and `runtime/shaders/smaa/{AreaTex.h,SearchTex.h}` from `dkb-pc-recomp`. Skipped `vk_dlss.c` (`GPL-2.0-or-later`) to maintain strict MIT license compliance.
- **ImGui Settings Menu (`runtime/ui/`, `runtime/third_party/imgui/`, `runtime/ui_text.c`):** Ported Dear ImGui + `imgui_impl_vulkan` and the two-pane menu (`F1`/`Esc`/`Minus+Plus`). Added Brazilian Portuguese (`pt-BR`) catalog & auto-detection alongside English (`en`), branded to `MTF Recomp`, and switched config persistence (`runtime/cfg.c`) to `mtf.ini`.
- **Gate F6 verified:** `TOTK_MENU_TEST=30:5 TOTK_MENU_TEST_SET=20:TOTK_UPSCALER=fsr,TOTK_SMAA=high,TOTK_MENU_LANG=pt-BR,SAVE=1 TOTK_PERF_OVERLAY=full` wrote `mtf.ini`, reloaded all 3 keys on the next run, and rendered all 7 menu tabs (`0.176 ms` CPU/frame) + performance overlay cleanly.

## 2026-11-15 — Phases F7 & F8: Diagnostics, Installer & Clean Release Packaging
- **Diagnostics (F7):** Ported MIT `runtime/flight.c`, `runtime/flight.h`, `runtime/rdoc.c`, `runtime/vk_headroom.c`, and `tools/flight_view.py` from `dkb-pc-recomp`; built `mtf-stage2.exe` cleanly in `0.5s`.
- **Installer & Release Packaging (F8):** Implemented Tkinter/CLI local-build setup & launcher (`installer/mtf_setup.py`) and clean release packager (`tools/make_release.py`).
- **Gate F8 verified:** `python tools/make_release.py` staged a clean source/launcher tree, passed `tools/check_content.py` (`0 violations`), and generated `build/release/mtf-recomp-0.1.0-dev-src.zip` (`2.79 MiB`).

## 2026-11-15 — VP9 `StartVp9Decoder` Black-Screen/Audio Fix & Setup Launcher Polish
- **Root cause & fix (`runtime/ov_codec.c`):** `nro0` uses `main`'s `movie::VideoDecoder`, which calls `nn::codec::StartVp9Decoder`. `StartVp9Decoder` was previously mapped to `ov_ok` (no-op), while `GetVp9DecoderSubmissionReadyEvent` signalled the ready event prematurely before `Start`; `MovieEventHandler` (`nro0+0xd16460`) cleared the event and posted to `MovieVideoHandler` (`nro0+0xd19c00`) before `Start` set its active flag, dropping the message and stalling both video and audio. Replaced `ov_ok` with `ov_vp9_start` to signal the submission-ready `SystemEvent` on `StartVp9Decoder`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.7s`), verified `TOTK_TRACE=audio` (`first nonzero output after 182 frames`, `0` underruns/overruns over `5.0 s`), and verified `TOTK_DUMP=120` (`work/dump_fixed/pass_0004_t0_2560x1440_43_rgba8srgb.png`) renders the 2560x1440 intro stadium movie frames.
- **Installer UX (`installer/mtf_setup.py`):** Disabled `Launch Game (Vulkan)` until `mtf-stage2.exe` exists and while a build is running, re-enabling it only after a successful build.

## 2026-11-15 — `nn::account::LoadProfileImage` Crash Fix, `1080p` Default & Menu Cleanup
- **Crash fix (`runtime/ov_system.c`):** Added `ov_load_profile_image` (`HOST_OVERRIDE` for `_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE`) returning a valid 64x64 baseline JPEG (`693` bytes), fixing `0xc0000005` at `sdk+0x184f20` (`mtf-stage2.exe+0x71b59b`).
- **Config & menu (`runtime/cfg.c`, `runtime/ui/menu.cpp`):** Excluded `mtf327680` from `dkb_output` (`2880x1620`) so `OUTPUT_RES` defaults to `1920x1080`, and removed the unimplemented `DLSS` choice from the upscaler menu (`FSR` / `Bilinear` only).
- **Launcher (`installer/mtf_setup.py`):** Guarded `on_launch()` against launching a second instance while `mtf-stage2.exe` is already running.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.7s`), `TOTK_TRACE=perf,audio` verified `60.0 fps` (`120 frames in 1.999 s`) and `0` audio underruns/overruns; `python tools/check_content.py` passed (`0 violations`).

## 2026-11-15 — `nvnCommandBufferSetViewportSwizzles` Fix & `F9` Frame Dump Support
- **NVN command buffer (`runtime/nvn_api_cmd.c`):** Implemented `nvnCommandBufferSetViewportSwizzles` (accepts identity swizzles and logs non-identity swizzles once) after `work/last_launch.log` captured `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented` following the `CallCommands` null-handle fix (`a0893e6`).
- **Diagnostics (`installer/mtf_setup.py`):** Enabled `TOTK_DUMP=key` and `TOTK_DUMP_DIR=work/f9_dump` by default in `on_launch()` so pressing `F9` in the game window writes a Vulkan frame dump.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.8s`), ran `TOTK_TRACE=perf,audio` (`60.0 fps`, `120 frames in 2.000 s`), verified `TOTK_MENU_TEST=210:2` with `mtf.ini` (`upscaler = bilinear`, `smaa = high`, `1920x1080`), and ran `python tools/check_content.py` (`0 violations`).

## 2026-11-15 — Indexed `NVN_DRAW_PRIMITIVE_QUADS` Fix (6x4 Black Triangle Grid)
- **Root cause & fix (`runtime/vk_exec_draw.c`):** Reproduced the post-title 3D scene via `TOTK_INPUT_SCRIPT=work/skip_to_menu.txt` (`TOTK_DUMP=600`). `pass 0010` (`24` deferred-lighting tiles, `prim 7 = NVN_DRAW_PRIMITIVE_QUADS`) used `op_draw_idx` (`DrawElements`), which did not expand 4-vertex quads into 6-index `TRIANGLE_LIST` pairs (`{i0, i1, i2, i0, i2, i3}`), causing Vulkan to drop the second triangle of every quad. Added quad index expansion in `op_draw_idx`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.4s`) and dumped frame `600` (`work/dump_post_fixed/pass_0010_t0_1706x960_122_r11g11b10f_tm.png`): the entire 3D tennis court, shadows, net, racket, ball, and sky render cleanly without black triangles.

### 2026-11-15 — VP9 Intro Movie 20 FPS / Audio Stutter Fix (Issue #3)
- **Root cause:** `ov_vp9_submit` in `runtime/ov_codec.c` did not signal `GetVp9DecoderSubmissionReadyEvent` when `oldest_wait(d) != NULL`, throttling the 60 FPS (`2560x1440`) VP9 player to 1 frame per ~50 ms (every `Release` + `Acquire` cycle) and starving the audio stream (`53,493 / 239,520` nonzero frames).
- **Fix:** Updated `ov_vp9_submit` to signal `submission-ready` iff `oldest_wait(d) != NULL` (and `clear_event` otherwise so the demuxer never overflows `QUEUED = 8`), and increased `libvpx` decoder threads to `4` for `w >= 1920`.
- **Verification:** `python tools/build_stage2.py --jobs 16` (`0.5s`), 8s `TOTK_TRACE=perf,codec,audio` run -> `60.0 fps`, `0` codec errors/drops, `0` audio underruns (`182,982 / 239,520` nonzero frames), PTS `5.07 s`.

### 2026-11-15 — UI Font Text & NVN2 Compute `LocalSize` Fix (Issue #4)
- **Root cause:** (a) All 78 NVN2 compute shaders (`sdf_font_compute.narc`, `skinning.narc`, `blendshape_compute.narc`) read `LocalSize` from NVN1's `ctl + 0x748` instead of NVN2's `ctl + 0x58`, resulting in `LocalSize(1,1,1)`; (b) `cs_font_0`/`cs_font_1` access a `VK_FORMAT_R32_UINT` storage image via float `OpTypeImage` (`r->format == 0`) + `OpBitcast`, requiring a `VK_FORMAT_R32_SFLOAT` storage view.
- **Fix:** Updated `tools/shaders/translate.py`, `runtime/gfx_shader.c`, and `runtime/vk_exec_draw.c` to read NVN2 compute `LocalSize` from `ctl + 0x58`, re-translated all 78 NVN2 `cs` shaders (`0 failed`), and added `R32_UINT`/`R32_SINT` $\rightarrow$ `R32_SFLOAT` storage view reinterpretation in `get_view` (`runtime/vk_exec_draw.c`).
- **Verification:** `mtf-stage2.exe` rebuilt (`1.5s`). Frame `320` (`work/dump_title_text_fix2/pass_0020_t0_2560x1440_43_rgba8srgb.png`) renders `Press ? + ?`, `© Nintendo / CAMELOT`, and `Ver. 1.1.1`; frame `600` (`work/dump_post_chars_fix/pass_0038_t0_2560x1440_43_rgba8srgb.png`) renders `Let's go over some basic button controls before jumping into the game.` and `?/? End practice`.

### 2026-11-16 — `NxUniformSizesBank` (`slot 25`) Fix for Title Key Art & UI Dialog Boxes (Issue #4)
- **Root cause:** `MetalUniformBounds` in `totk_xlate` bounds-checks dynamically indexed reads from `c[3]..c[16]` against `NxUniformSizesBank` (`slot == 25`, 16x `uint32_t` bound sizes). `runtime/vk_exec_draw.c` (`ubo_info`) fell through to `zero_info(max)` for `slot == 25`, causing dynamic UV/UI UBO reads to return `0.0`.
- **Fix:** Implemented `slot == 25` in `ubo_info` and `rs_build` (`runtime/vk_exec_draw.c`).
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.3s`); frame `320` renders the full *Mario Tennis Fever* title key art and frame `600` renders the tutorial dialog box and `▼` indicator cleanly.

### 2026-11-16 — Character Rendering Fix in Post-Title / Tutorial Scene (Issue #10)
- **Root cause:** `f_7002fa5700` / `f_7003057224` in `nro0` bake per-shape NVN command buffers on a shared `NVNcommandBuffer` (`ctl_size = 256`, `game_size = 8192`) and check `total_used = (end_cmd - start_cmd) + (end_ctl - start_ctl) >= 9`. Because `nvnCommandBufferBeginRecording` (`runtime/nvn_api_cmd.c`) never fired `OUT_OF_COMMAND_MEMORY` / `OUT_OF_CONTROL_MEMORY` callbacks when `hw_used` / `ctl_used` reached `game_size` / `ctl_size`, `Get{Command,Control}MemoryUsed` clamped at `game_size` / `ctl_size` (`delta = 0 < 9`) after the first 2–4 shapes, causing `f_7003057224` to discard the baked draw command handles (`odd_c8 = 0`) for Mario, Luigi, Lakitu's body, and Umpire Toad. Additionally added `spirv_rewrite_int_inputs` (`runtime/spirv_rewrite.c`, `runtime/vk_pipeline.c`) for integer vertex attribute formats (`UINT`/`SINT`).
- **Fix:** Trigger `memory_event` in `nvnCommandBufferBeginRecording` (`runtime/nvn_api_cmd.c`) when `hw_used + 1024 > game_size` or `ctl_used + CTL_PER_RECORDING > ctl_size` so the guest replenishes command/control memory before recording.
- **Verification:** Rebuilt `mtf-stage2.exe` cleanly and dumped frame `800` (`work/dump_800_fix/pass_0037_t0_2560x1440_43_rgba8srgb.png`): Mario, Luigi, Lakitu, and Umpire Toad render completely with materials and shadows.

### 2026-11-16 — Darkened Output / sRGB-to-Linear Present Fix (Issue #7)
- **Root cause:** `nvnQueuePresentTexture` in MTF presents `VK_FORMAT_R8G8B8A8_SRGB` into a `VK_FORMAT_B8G8R8A8_UNORM` host swapchain (`runtime/vk_present.c`), causing `vkCmdBlitImage` / `vk_fsr_record` / `vk_color_record` to decode sRGB to linear without re-encoding on write.
- **Fix:** Reinterpret `*_SRGB` presented textures as `*_UNORM` in `runtime/vk_present.c` and force `vk_color_record` (`runtime/vk.h`, `runtime/vk_color.c`) passthrough when `vfmt != fmt` so raw sRGB bytes reach the `UNORM` swapchain unmodified.
- **Verification:** Rebuilt `mtf-stage2.exe` and captured `work/shots_fix7/f00795.png` via `TOTK_SHOTS=780-810:15`: brightness and gamma match the reference output.

### 2026-11-16 — Bottom Tutorial Hint Banner Center Background Fix (Issue #11)
- **Root cause:** In `pass 0036` (`pipe 4`, `prog 130`), the center quad of the bottom hint banner uses NVN2 vertex shader `c381fe4e8be846a3` (SPIR-V FNV `0x67e197ca7ba86ada`) with `vp_c3[34].w == 0`. `totk_xlate` dropped the `0x970..0xc30` `(vp_c3[34].w & 2) == 0` `else` block that reads `vp_c3[29..32]`, leaving `alpha = clamp(-250.0 * vp_c3[13].w, 0, 1) = 0.0`.
- **Fix:** Added `spirv_ui2d_color_fix` (`runtime/spirv_rewrite.h`, `runtime/spirv_rewrite.c`, `runtime/gfx_spirv.c`) to reconstruct the bilinear `vp_c3[29..32]` corner-color interpolation for `0x67e197ca7ba86ada`.
- **Verification:** Rebuilt `mtf-stage2.exe` and dumped frame `1200` (`work/dump_1200_fix11/pass_0037_t0_2560x1440_43_rgba8srgb.png`): the bottom hint banner now renders seamlessly across the entire width without any center gap.

### 2026-11-17 — Unification with External MTF Release (`v0.1.0+6caba99`) & Fixes for Issues #12 and #13
- **Phase 1 (Shader Profile, `spirv_txd_transpose`, & NVN2 Key Caching — Issue #12):**
  - Imported `profiles/0400D4201C6EA000/shader_answers.tsv` (`2,520` entries), `tools/shaders/int_inputs.py`, and updated `tools/shaders/translate.py` (`load_answers`, `sbuf_align=16`, `tess_mode=0x202`, `code[:ctl[0x38]]`, `--mvprev`).
  - Added `spirv_txd_transpose` (`runtime/spirv_rewrite.{h,c}`, `runtime/gfx_spirv.c`) and cached the 8-byte SHA-256 key inside `prog_t.owned_control` (`runtime/nvn_api_obj.c`, `runtime/gfx_shader.c`) with power-of-2 log throttling.
- **Phase 2 (`sampled_format` View Reinterpretation, Shadow Scale & PCSS — Issue #13):**
  - Added `nvn_format` to `VkTexImage` (`runtime/vk.h`, `runtime/vk_texture.c`) and implemented `sampled_format` in `get_view` (`runtime/vk_exec_draw.c`), fixing the sRGB $\rightarrow$ linear double-gamma bug when `pass 0036` (`2560x1440` `RGBA8_UNORM`) samples `pass 0035` (`1706x960` `RGBA8_SRGB`) through an `NVN_FORMAT_RGBA8` view.
  - Added `SHADOW_SCALE` (`1..4x`) and `PCSS` (`pcss_params`, `shadow_spread_params`) across `runtime/cfg.c`, `runtime/vk_texture.c`, `runtime/vk_exec.{h,c}`, `runtime/vk_exec_xfer.c`, `runtime/vk_exec_draw.c`, and `runtime/gfx_c0.{h,c}`, plus `hdr_compose.comp` (`hdr_compose_spv.h`).
- **Phase 3 & 4 (Resolution/Render Scale, BFTTF De-obfuscation, `MTF_*` Config Prefix, DLAA/Amiibo/Installer/CMake):**
  - Added `runtime/ov_resolution.c` hooked into `nn::vi::CreateLayer` and `nn::vi::QueryDisplayBufferMaxResolution` (`runtime/ov_gfx.c`), BFTTF XOR key de-obfuscation (`runtime/ov_system.c`), `MTF_*` prefix support in `runtime/cfg.c`, and imported `runtime/amiibo.{c,h}`, `runtime/nfp.{c,h}`, `runtime/diag_watch.{c,h}`, `runtime/ov_audioout.c`, `runtime/vk_dlaa.{c,h}`, `recomp/{glue,lift,module_scan,nro_capture,nro_lift}.py`, `CMakeLists.txt`, and `installer/`.
- **Verification:** Rebuilt `mtf-stage2.exe`, verified frame `1200` (`work/dump_1200_fix12_13/pass_0037_t0_2560x1440_43_rgba8srgb.png`) and 6s boot/shader run (`work/test_merge_run.log`), and ran `python tools/check_content.py` (`0 violations`).

### 2026-11-17 — Tessellation (`TCS`/`TES`), Multi-Viewport Shadow Cascades (`ViewportMaskNV`), `OP_DRAW_IND_MULTI`, and Compute `c0+0x4`
- **Changes:**
  - Added compute dispatch dimensions at `c0 + 0x4` in `runtime/gfx_c0.c` (`gfx_c0_fill`) so SDF glyph compute shaders read their final workgroup bounds without leaving stray edge lines.
  - Added `OP_COPY_IMAGE_STRIDE`, `OP_PATCH_SIZE`, and `OP_DRAW_IND_MULTI` (`DrawArraysIndirect`, `MultiDraw{Arrays,Elements}IndirectCount`) in `runtime/nvn_obj.h`, `runtime/nvn_api.h`, `runtime/nvn_api_cmd.c`, `runtime/vk_exec.{h,c}`, `runtime/vk_exec_xfer.c` (`copy_image_stride`), and `runtime/vk_exec_draw.c`.
  - Added per-viewport scissor tracking (`p_scissor.index`, `vp_n`, `scissor_n[16]`, `scissor_mask`, `vk_exec_scissor_at`, `pass_viewports`, `viewport_at`) and `VK_NV_geometry_shader_passthrough` / `VK_NV_viewport_array2` + `viewport_relative_layer` SPIR-V injection (`runtime/vk_device.c`, `runtime/vk_pipeline.c`).
  - Added `TCS`/`TES` tessellation pipeline support (`VkPipeState.{tcs,tes,patch_size}`, `GKey.{tcs,tes,patch}`, `mid_stages`, `VkPipelineTessellationStateCreateInfo`) and `TESS` / `GEOMETRY` config knobs (`runtime/cfg.c`, `runtime/vk_exec_draw.c`, `runtime/vk_pipeline.c`).
- **Verification:**
  - Smoke-tested `tools/shaders/translate.py --xlate ".../dmsh_xlate.exe" --limit 20` (`20/20` translated, `0 failed`).
  - Built `mtf-stage2.exe` cleanly (`0` errors), ran `mtf-stage2.exe --seconds 8` with `TOTK_XLATE=.../dmsh_xlate.exe` (`exit 0`, 60 FPS on RTX 5070 Ti), and verified `python tools/check_content.py` (`0 violations`).

### 2026-11-17 — `vk_dlaa.c` (DLAA / DLSS / FXAA-Off) Integration
- **Changes:**
  - Wired `runtime/vk_dlaa.c` into `tools/build_stage2.py`, `runtime/vk_device.c` (`vk_dlaa_instance_exts`, `vk_dlaa_device_exts`, `vk_dlaa_init`), `runtime/vk_exec.c` (`vk_dlaa_pass`, `vk_dlaa_submission_end`), `runtime/vk_exec_draw.c` (`vk_dlaa_viewport`, `vk_dlaa_skip_draw`, `vk_dlaa_fs_swap`, `vk_dlaa_draw`), `runtime/vk_present.c` (`vk_dlaa_frame`), `runtime/vk_texture.c` (`vk_sampler_dlss_bias`), and `runtime/ov_resolution.c` (`ov_resolution_sizes`, `res_sizes`).
  - Added `gfx_shader_reflection` and `gfx_shader_own` in `runtime/gfx.h` / `runtime/gfx_shader.c`, `"hs"`/`"ds"` tessellation stage recognition in `reflection_valid` / `reflect`, `DLAA`/`DLAA_OBJMV`/`DLSS`/`FXAA` knobs in `runtime/cfg.c`, and weak `vk_mvobj_execute` in `runtime/stage2_stubs.c`.
- **Verification:**
  - Built `mtf-stage2.exe` (`0` errors) and ran with `MTF_DLAA=1 --seconds 8` (`work/test_dlaa_err.log`): NVIDIA NGX core loaded (`dlaa: NGX up`) and exited cleanly (`0`).
  - Verified `python tools/check_content.py` (`0 violations`).

### 2026-11-17 — ACES HDR10 Inverse-Tonemap Compose (`vk_hdr_draw` + `hdr_compose.comp`)
- **Changes:**
  - Integrated MTF's ACES tonemap input tracker (`vk_hdr_track`, `vk_hdr_draw`, `vk_hdr_image_gone`, `has_aces` / `is_tonemap` constant matcher) and `hdr_compose_spv.h` (`hdr_compose.comp` compute shader) into `runtime/vk_hdr.c`, `runtime/vk.h`, `runtime/vk_texture.c`, and `runtime/vk_exec_draw.c`.
  - `vk_hdr_record` now uses `H.comp` (`hdr_compose.comp` $\rightarrow$ `R16G16B16A16_SFLOAT` PQ Rec. 2020) whenever the game's ACES tonemap draw (`light` + `bloom` + `exposure`) is active, falling back to `hdr_encode_spv` for menus/movies.
- **Verification:**
  - Built `mtf-stage2.exe` cleanly (`0` errors) and ran with `TOTK_HDR=1 --seconds 8` (`work/test_hdr_err.log`): HDR10 swapchain (`format 64`), display metadata (`peak 1000 nits, paper white 203`), and clean exit (`0`).
  - Verified `python tools/check_content.py` (`0 violations`).

### 2026-11-20 — Lightning Racket `SSign` GPU Hang & Multi-Match `pipe_worker` Driver Crash
- **Changes:**
  - `runtime/vk_pipeline.c` (`spirv_fixup`): Rewrites `GLSL.std.450` `SSign(int32)` (`ExtInst 5`) to `SClamp(x, -1, 1)` (`ExtInst 45`), injecting `int32` `-1`/`1` constants when absent. Fixes the `VK_ERROR_DEVICE_LOST` (`instruction pointer (invalid)`) GPU hang triggered by vertex shader `0fa55a01f31ef9a6` during Lightning Racket matches on NVIDIA Blackwell (`RTX 5070 Ti`, driver `610.88`).
  - `runtime/vk_pipeline.c` (`vk_pipeline_async_enable`): Defaults `MTF_PIPE_SYNC=1` on NVIDIA (`vendorID == 0x10de`) when `!P.gpl` (`MTF_PIPE_SYNC=0` overrides), avoiding the `nvoglv64.dll+0x10ea90` (`0x8` null-read) access violation in `create_graphics` on background `pipe_worker` threads across 3–4+ consecutive matches.
  - `runtime/vk.h`, `runtime/vk_device.c`, `runtime/vk_exec_draw.c`, `runtime/vk_rec.{h,c}`, `runtime/gfx_stubs.c`: Added draw/dispatch ring-buffer breadcrumbs + 4s hang watchdog + optional `VK_NV_device_diagnostic_checkpoints` (`MTF_PASS_CRUMBS=1`) + `MTF_SKIP_VS` diagnostic filter.
- **Verification:**
  - Standalone `spv_repro.exe` on `RTX 5070 Ti` (`610.88`): `0fa55a01f31ef9a6.spv` hung with `vkQueueWaitIdle: -4`, and passed (`vkQueueWaitIdle: 0`) after rewriting `SSign` to `SClamp(x, -1, 1)`.
  - User verified Lightning Racket match runs without freezing/crashing, and `MTF_PIPE_SYNC=1` passed 10 consecutive matches with distinct characters and rackets.
  - Rebuilt `build/fork/bin/mtf-recomp.exe` (`0` errors) and verified `python tools/check_content.py` (`0` violations).

### 2026-11-20 — Tier 2 / Stage 2.1: Memory, VRAM Headroom, Lazy Heap & Sparse Mirror Copy
- **Changes:**
  - `runtime/vk_headroom.c`, `CMakeLists.txt`, `runtime/gfx_stubs.c`, `runtime/cfg.c`: Ported `vk_headroom.c` (`MTF_VRAM_HEADROOM=1` default) from `dkb-pc-recomp` (`4e7f9b2`, MIT) to pre-allocate the next 256 MiB VMA block per memory type in background and back off above 85% of VRAM budget.
  - `runtime/platform/platform.h`, `runtime/platform/win32.c`, `runtime/host.h`, `runtime/memory.c`, `runtime/romfs.c`, `runtime/fs_host.c`, `runtime/vk_memory.c`, `runtime/cfg.c`: Ported `MTF_HEAP_LAZY=1` (default) via `fault_veh` 64 KiB on-demand commit + `mem_touch` before host file reads / pool imports.
  - `runtime/vk_memory.c`, `runtime/mirror_sparse_spv.h`, `runtime/shaders/mirror_sparse.comp`, `runtime/vk.h`, `runtime/vk_device.c`, `runtime/cfg.c`: Ported `MTF_SPARSE_MIRROR_COPY` (compute dispatch for small write-back ranges, `auto` on NVIDIA) from `dkb-pc-recomp` (`4e7f9b2`, MIT).
  - `runtime/platform/win32.c` (`plat_mem_avail_commit`), `runtime/vk_memory.c`, `runtime/cfg.c`: Added clean-room Windows commit headroom (`MTF_COMMIT_HEADROOM_MIB=512` default) using `GlobalMemoryStatusEx` (`ullAvailPageFile`) to keep pools imported instead of mirroring when system commit charge is tight.
- **Verification:**
  - Built `build/fork/bin/mtf-recomp.exe` cleanly via `ninja` (`0` errors).
  - 6s smoke test (`build/fork/bin/mtf-recomp.exe`, `alive=True`, commit `5076 MB`) confirmed `build/fork/bin/logs/mtf.log` entries:
    - `mem: small write-back ranges are copied by one compute dispatch a pool (MTF_SPARSE_MIRROR_COPY=auto)`
    - `mem: Windows commit headroom 512 MiB (MTF_COMMIT_HEADROOM_MIB)`
    - `vk: headroom: a 256 MiB block of memory type ... allocated ahead (6 so far; 13 allocations of 32 MiB or more on other threads)`
  - A/B boot check for `HEAP_LAZY`: `HEAP_LAZY=0` (`5072 MB` at 6s) vs `HEAP_LAZY=1` (`5079.3 MB` at 6s); commit savings take effect when guest expands heap via `svcSetHeapSize`.
  - Verified `python tools/check_content.py` (`0` violations).

### 2026-11-21 — Tier 2 / Stage 2.2: Multi-Threaded Parallel Replay (`MTF_REPLAY_THREADS`)
- **Changes:**
  - `runtime/vk_exec.c`, `runtime/vk_exec.h`, `runtime/cfg.c`: Ported parallel pass slicing (`MTF_REPLAY_THREADS`, default `6` on >=12 logical CPUs / `4` on >=8 CPUs / `0` on AMD) from `dkb-pc-recomp` (`4e7f9b2`, MIT). Large passes (`>= 256` draws) are sliced into secondary command buffers (`48..1024` draws) and recorded across worker threads in parallel, then executed via `vkCmdExecuteCommands`.
  - `runtime/vk_exec_draw.c`: Isolated per-thread draw/memo/descriptor state into `DrawCtx` (`tls_dc` + `vk_exec_draw_slice_enter`, `vk_exec_draw_slices_begin`, `vk_exec_draw_slice_ctx`, `vk_exec_draw_slice_view`, `vk_exec_draw_slices_end`, `vk_exec_draw_rebase`) and `PlatOnce` for `dummies_init`, preserving all MTF-specific hooks (ACES HDR, DLAA, breadcrumbs, NVN2).
  - `runtime/vk_exec_xfer.c`: Added occlusion-query inheritance support (`vk_exec_oq_sliceable`, `vk_exec_oq_begin_outside`, `vk_exec_oq_pass_ended`).
  - `runtime/vk_usnap.c`, `runtime/vk_memory.c`: Added `vk_usnap_drops`/`vk_usnap_view`/`vk_usnap_quiet` and `vk_mem_threads`/`vk_mem_count_quiet`/`wb_locals` thread-local write-back lists so slice workers run lock-free in the common case.
  - `runtime/vk_rec.{h,c}`: Added `rec_capture_*` and `rec_owns`.
  - Evaluated `vk_spec.c` + `vk_exec_draw_helper.c` (`MTF_REPLAY_PAR`): deferred as optional since `MTF_REPLAY_THREADS=6` already parallelizes heavy render passes without duplicating `vk_exec_draw.c`.
- **Verification:**
  - Built `build/fork/bin/mtf-recomp.exe` cleanly via `ninja` (`0` errors).
  - 6s smoke test (`alive=True`, 0 `FATAL`) confirmed in `build/fork/bin/logs/mtf.log`: `vk: passes of 256 or more draws are recorded in slices of up to 1024 draws into secondary command buffers (MTF_REPLAY_THREADS=6, the default: recorded by that many threads)`.
  - Verified `python tools/check_content.py` (`0` violations).

### 2026-11-21 — Tier 2 / Stage 2.3: Fragment NaN Guard (`MTF_NAN_GUARD`) & MIT Credits
- **Changes:**
  - `runtime/spirv_nan_guard.{c,h}`, `CMakeLists.txt`, `runtime/vk_pipeline.c`, `runtime/cfg.c`: Ported `spirv_nan_guard` (`MTF_NAN_GUARD`, `0|zero|mark`, default `0`) from `dkb-pc-recomp` (`4e7f9b2`, MIT) into `make_module()` to sanitize NaN fragment shader color outputs when enabled.
  - `THIRD_PARTY_NOTICES.md`, `docs/roadmap.md`, `docs/perf-methodology.md`: Consolidated all Tier 2 MIT attributions for `dkb-pc-recomp` (`4e7f9b2`).
- **Verification:**
  - Built `build/fork/bin/mtf-recomp.exe` cleanly via `ninja` (`0` errors).
  - 6s smoke test with `MTF_NAN_GUARD=zero` (`alive=True`, 0 `FATAL`) confirmed `[host] vk: NaN guard on fragment outputs: zero (MTF_NAN_GUARD)`.
  - Verified `python tools/check_content.py` (`0` violations).

### 2026-11-21 — `F1` Menu: Control Remapping (Keyboard & Gamepad) + Tier 2 Options
- **Changes:**
  - `runtime/input_map.{c,h}`, `runtime/input.{c,h}`, `runtime/cfg.c`: Ported full action-to-key/gamepad mapping tables (`MTF_BIND_A..SHAKE`, 2 keyboard slots + gamepad button per action, live reload on `cfg_version()` change) and `input_capture()` / `input_key_char()` / `input_pad_label()`.
  - `runtime/ui/binds_page.{cpp,h}`, `runtime/ui/option_rows.{cpp,h}`, `runtime/ui/menu.cpp`, `runtime/ui_text.{h,c}`, `CMakeLists.txt`: Added **Controller** and **Keyboard** sub-tabs under **Controls** with interactive 5s modal capture, clear (`Delete`/`Backspace`/`Y`), auto-swap notifications, and "Reset button bindings" in EN + PT-BR; exposed `MTF_REPLAY_THREADS`, `MTF_VRAM_HEADROOM`, `MTF_HEAP_LAZY`, `MTF_SPARSE_MIRROR_COPY`, and `MTF_NAN_GUARD` in **Advanced**.
  - `docs/roadmap.md`: Added **Multiplayer Online / Link Play** (`nn::ldn` / `nn::socket` HLE) as the next top priority.
- **Verification:**
  - Built `build/fork/bin/mtf-recomp.exe` cleanly via `ninja` (`0` errors).
  - 6s smoke test (`alive=True`) and `python tools/check_content.py` (`0` violations) passed.


### 2026-11-21 — Merge Multiplayer (`feat/multiplayer-ldn`) + Release `v0.1.0`
- **Changes:**
  - Merged `feat/multiplayer-ldn` into `main` (`5846960`): multi-gamepad routing (`P1..P4`) & `F1` Controls UI, `nn::ngc::ProfanityFilter` vtable crash fix, split-screen DLAA aspect guard (`is_full_aspect`), and `F1 -> Multiplayer` browser streaming (`CloudFlare WebSocket`, `WebRTC UDP` via `libdatachannel` + `Mbed TLS`, and `Local Host` with Win32 clipboard copy and `cloudflared.exe` installer).
  - Updated `tools/make_release.py` to include `CMakeLists.txt` in `INCLUDE_FILES`.
- **Verification:**
  - Audited all `1,956` git-tracked files (`Suspicious tracked files: []`) and ran `python tools/check_content.py` (`0` violations).
  - Built `build/fork/bin/mtf-recomp.exe` on `main` cleanly via `ninja` (`0` errors) and packaged `build/release/mtf-recomp-0.1.0-src.zip`.


### 2026-11-22 — GUI Installer (`MTF-Recomp-Setup.exe`) Fixes & `v0.1.0` Release Prep
- **Changes:**
  - `CMakeLists.txt`: Bundled `libwinpthread-1.dll` into `MTF_DEPS_DLLS` so `libvpx-1.dll` loads cleanly during `--capture-nro` and runtime execution.
  - `installer/steps.py`, `installer/layout.py`: Added `CREATE_NO_WINDOW` to subprocess calls, prioritized `build/toolchain/xlate/dmsh_xlate.exe` (`0.1.0+6caba99`) over stale `build/xlate_nx/dmsh_xlate.exe`, and cleared stale `shaders/` before re-translating.
  - `installer/steps.py`, `installer/gui.py`, `installer/guilogic.py`, `installer/cli.py`, `installer/text/{en,pt-BR}.json`: Removed the obsolete `fonts` step and `??` button-icon warning (button glyphs are generated dynamically by `ov_font.c` / `glyph_draw.c`), and bundled `installer/assets` + `installer/text` into `MTF-Recomp-Setup.exe`.
  - `runtime/ui/menu.cpp`: Renamed `CloudFlare (recommended)` to `Cloudflare (WebSocket)` alongside `WebRTC (Low Latency UDP)` and `Local Host`.
  - `README.md`, `docs/port-status.md`: Added Multiplayer & Remote Play comparison table (transport, use cases, IP privacy, expected delay) and GUI Installer quick-start instructions.
- **Verification:**
  - Verified `python -m installer.gui --selftest` (`exit 0`), `python tools/check_content.py` (`exit 0`), PyInstaller `MTF-Recomp-Setup.exe` build (`exit 0`), and `python tools/make_release.py --version 0.1.0`.


### 2026-11-22 — `v0.1.1`: `F1 -> Cheats`, NVIDIA GPL Crash Fix & Installer `MENU_LANG`
- **Changes:**
  - `profiles/0400D4201C6EA000/cheats_profile.h`, `runtime/cheats.{c,h}`, `runtime/cfg.c`, `runtime/gfx_present.c`, `runtime/ui_text.{h,c}`, `runtime/ui/menu.cpp`: Added **Cheats** tab in `F1` (`Unlock Everything` / `Disable All` + live toggles for Characters, Costumes & Colors, Rackets, Courts, and Cups & Modes) backed by non-destructive in-memory bitfield snapshots in `nro0` `.bss`.
  - `runtime/vk_caps.c`, `runtime/vk_pipeline.c`, `runtime/vk_exec.c`: Re-enabled `VK_EXT_graphics_pipeline_library` (`GPL` + `fast-link`) by default and guarded non-GPL NVIDIA fallback paths against multi-threaded `vkCreateGraphicsPipelines` crashes (`nvoglv64.dll+0x10ea90`).
  - `installer/{gui,guilogic,cli,steps}.py`: Passed the installer's selected language (`en` / `pt-BR`) via `--lang` to `Finish.run()` so `mtf.ini` starts with `MENU_LANG` matching the setup language.
  - `VERSION`, `README.md`, `docs/port-status.md`: Bumped to `0.1.1` and documented `F1 -> Cheats`.


### 2026-11-23 — `v0.1.2`–`v0.1.4`: RecompLauncher SDK, GitHub Actions Releases, Stream Quality, Update Notifier & `spirv_rewrite_inputs` Fix
- **Changes:**
  - `recomplauncher.json`, `installer/{cli,exe_entry,layout}.py`, `runtime/{gfx_present,input}.c`: Added RecompLauncher SDK integration (Levels 1–3, headless `--prepare`, `RECOMPLAUNCHER_MODE=bigpicture` fullscreen, `RECOMPLAUNCHER_PAD` initial button icons) and guarded missing `git` in `dev_layout`.
  - `.github/workflows/release.yml`, `tools/make_release.py`: Added clean GitHub Actions Windows `--onedir` release workflow (`MTF-Recomp-v<ver>-Windows-x64.zip` trimmed of dev `docs/`/`tests/`/`installer/*.py`, plus `mtf-recomp-<ver>-src.zip`).
  - `runtime/{stream_webrtc.cpp,stream_host.c,gfx_present.c,cfg.c,ui/menu.cpp}`: Switched WebRTC media DataChannel to ordered partial reliability (`150 ms`), added congestion delta skipping + keyframe recovery, `25s` WS keepalive, automatic WebSocket fallback when UDP is blocked, `Performance`/`Balanced`/`Quality` presets (`STREAM_QUALITY`), and 3s player connect/disconnect toasts.
  - `runtime/{update_check.h,update_check.c,ui/menu.cpp,ui_text.*}`, `CMakeLists.txt`: Added asynchronous GitHub release checker on boot with one-time 5s top toast (`UPDATE_NOTIFIED_TAG` in `mtf.ini`) and `F1` menu reminder + download button.
  - `runtime/vk_pipeline.c`: Fixed `spirv_rewrite_inputs` emitting `OpType*` after `OpVariable` (`nvoglv64.dll+0x10ea90` crash in Free Play character select), kept synchronous pipeline builds on NVIDIA, and re-enabled `VkPipelineCache`.
  - `runtime/gfx_window_win.c`: Added `Alt+F4` (`WM_SYSKEYDOWN`) window close support.
  - `README.md`, `CONTRIBUTING.md`, `docs/*`: Upgraded README with screenshots (`docs/images/`), copyright notice, AI disclosure, controls, and bug-reporting guide; standardized all `docs/*.md` to English.
- **Verification:**
  - `python tools/build_fork.py`, `python -m installer.gui --selftest`, `python tools/check_content.py`, and GitHub Actions release builds (`v0.1.3`, `v0.1.4`) all passed cleanly.


### 2026-11-24 — `v0.1.5`–`v0.1.6`: Gate F5 Zero-Validation, AMD Compatibility, HDR10 Calibration & Draggable Sliders
- **Changes:**
  - `runtime/gfx_shader.c`, `runtime/vk_exec_draw.c` (`v0.1.5`): Added `ensure_storage_caps` (`StorageImageReadWithoutFormat` / `StorageImageWriteWithoutFormat`) and storage-image format reinterpretation (`R32_UINT`/`R32_SINT` -> `R32_SFLOAT`), closing Gate F5 (`0` validation errors / `0` warnings on NVIDIA).
  - `runtime/vk.{h}`, `runtime/vk_device.c`, `runtime/vk_exec_draw.c`, `runtime/vk_pipeline.c`: Added `has_gs_passthrough` detection, skipped `ShaderViewportMaskNV` (`5255`) / `BuiltIn ViewportMaskNV` (`5253`) on devices lacking `VK_NV_viewport_array2`, and implemented `lower_gs_passthrough()` (plus multi-struct `CullDistance` stripping in `spirv_fixup()`) to convert `GeometryShaderPassthroughNV` (`5251`) `gl_Layer` shaders into standard Vulkan Geometry Shaders forwarding `Location 0` — fixing the AMD match-entry crash and the 3D light-blue atmosphere wash.
  - `runtime/ui/menu.cpp`: Auto-disabled `MTF_DLAA` / DLSS rows when `!vk_dlaa_supported()`; locked `MTF_RENDER_SCALE` to `100` and `MTF_UPSCALER` to `bilinear` (disabling `MTF_SHARPNESS` alongside SDR color controls) when `HDR` is active.
  - `runtime/vk_pipeline.c`: Added a console log message in `vk_pipeline_prewarm()` when compiling queued pipelines on first launch or GPU/driver change.
  - `runtime/shaders/hdr_compose.comp`, `runtime/hdr_compose_spv.h`, `runtime/vk_hdr.c`: Removed `lift_shadows()` over-brightening, set `knee(0.97f)` with a `2.0x` peak-to-paper-white roll-off cap, and removed the fragile `1.5/255` pixel-match check so `Game Brightness` and `Display Peak Nits` apply reliably live during 3D gameplay.
  - `runtime/ui/option_rows.cpp`: Replaced `opt_slider` `< value >` steppers with draggable `ImGui::SliderFloat` bars (supporting `Ctrl+Click` keyboard numeric entry).
  - `README.md`, `docs/port-status.md`, `docs/status-details.md`: Added realistic percentage-based status summary (`SavageWindRecomp` style) and dedicated `status-details.md` breakdown.
- **Verification:**
  - `python tools/build_fork.py`, `MTF_GPU_INDEX=1` + `MTF_VK_VALIDATION=1` boot smoke test (`FATAL: 0`), user-verified in-match AMD boot/colors and live HDR sliders on NVIDIA, and `python tools/check_content.py` (`exit 0`).


### 2026-11-24 — `v0.1.7`: Self-Contained Toolkit Release, PR #15 (RecompLauncher `.nup`), DLAA Probe Fix & Firefox Stream Fix
- **Changes:**
  - Merged **PR #15** (`Xarishark:recomplauncher-support`): `recomplauncher.json` now uses `inputs` accepting `.nup` (base + 1.1.1) or extracted 1.1.1 folders (`nup_build_id: 104bce6874603701`), fixes language codes (`es`, `fr`, `de`, `it`, `ja`, `ko`), `FPS_LIMIT=unlimited`, `VK_PRESENT=mailbox`, optional `nvidia.dlss`, `MTF_UPDATE_CHECK=0`, and `keeps_copy` (`--game` optional in `installer/cli.py --prepare`).
  - `runtime/vk_dlaa.c`: Fixed `vk_dlaa_supported()` returning `0` on NVIDIA GPUs when booting with `MTF_DLAA=0`.
  - `runtime/stream_hwenc.cpp`, `runtime/stream_host.c`: Added explicit `MF_MT_DEFAULT_STRIDE` for NV12 input, used `f.visibleRect` in WebCodecs `drawImage`, and fell back to H.264/VP8 on Firefox clients to prevent the green-padding crop bug while keeping AV1 on Chrome/Edge.
  - `VERSION`, `docs/port-status.md`, `docs/devlog.md`: Bumped to `0.1.7` and packaged the self-contained Windows x64 archive (`--bundle-dir build/release-bundle`).
