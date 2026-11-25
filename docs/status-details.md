# MTF Recomp — Status Breakdown

**Last updated:** 2026-11-25 (`v0.1.8`)  
**Target:** *Mario Tennis Fever* (Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`)

This page explains how each percentage on the main status summary is calculated and what has been verified vs. what remains partial or GPU-specific.

---

## Percentage Methodology

| Area | Current | What the percentage means | Verified & Working | Known Gaps / Pending |
| :--- | :---: | :--- | :--- | :--- |
| **ARM64 Static Lifting (`main` + `sdk` + `nro0`)** | **99%** | Functions lifted cleanly to C, compiled, and linked into `mtf-recomp.exe` | 110,464 lifted functions (`1,589` `main` + `47,907` `sdk` + `60,968` `nro0`); 119,955 Unicorn diff cases passed; 0 unsupported in `main`+`sdk` | 1 remaining `ldst` edge case in `nro0` (non-blocking in tested modes) |
| **Horizon OS HLE & Kernel** | **95%** | OS primitives, FS/RomFS, threading, preemption, and `ro::LoadModule` | Clean boot, save persistence (`profile/`), async preemption, `nro0` runtime unpack, in-memory Cheats | Network/online Nintendo services stubbed locally (no NSO) |
| **Vulkan 1.3 Renderer (NVIDIA)** | **96%** | Frame pacing, shader translation, post-processing, and visual parity | `35,045 / 35,045` shaders translated; Gate F5 **0 validation errors / 0 warnings** on RTX 5070 Ti; HDR10, FSR 1, DLAA/DLSS, Frame Gen, 60 Hz sim | Occasional minor shader/input type warnings logged in `answers.tsv` |
| **Vulkan 1.3 Renderer (AMD)** | **86%** | Boot, match entry, colors, and effects on AMD GPUs | Boots and enters matches without crashing on AMD (`amdvlk64.dll 24.30.66`); DLAA/DLSS auto-disabled; `GeometryShaderPassthroughNV` & `ViewportMaskNV` (`MultiViewport` when `SHADOW_SCALE >= 2`) lowered to standard Vulkan GS; `MTF_GPL_DRAW_LINK=0` & `MTF_PIPE_PREWARM_BLOCK_MS=3000` | Ball trail interior on A/B/Y shots does not fill on AMD RDNA 2 (`24.30.66`); cascade 0 only on `SHADOW_SCALE=1` / Low preset to preserve iGPU FPS |
| **Audio (`audren` REV15) & Video (VP9)** | **98%** | Music, SFX, voice clips, and cutscenes | SDL3 `48000 Hz` 96-voice `audren` REV15 + `libvpx` VP9 `2560x1440` cutscenes | Minor audio crackle possible under heavy shader compilation stutter |
| **UI, Controls & HDR Calibration** | **95%** | In-game `F1` overlay, localization (`en` / `pt-BR`), input remapping, HDR10 | Draggable sliders + `Ctrl+Click` typing; automatic `Render Scale 100%` + `Bilinear` lock when HDR is active; live `Game Brightness` / `Peak Nits` / `UI Brightness`; SDL3 controllers + keyboard remap | First launch / GPU switch pipeline prewarm blocks window opening for several seconds (console progress message added) |
| **Multiplayer & Remote Play** | **92%** | Local `P1`–`P4` + browser streaming | WebRTC UDP DataChannel + automatic WebSocket fallback, Cloudflare Tunnel (`cloudflared.exe`), Local Host, `AV1` (Chromium) / `H.264` (Firefox) + `Opus`, `Performance`/`Balanced`/`Quality` presets | Requires user-hosted `cloudflared.exe` for WAN IP privacy; strict symmetric NATs fall back to WebSocket; **Firefox** supports **H.264** (VP8 fallback) only (`AV1` requires a Chromium browser) |
| **Packaging, Launcher & Updates** | **98%** | Self-contained Windows installer, RecompLauncher SDK, updater | Self-contained `MTF-Recomp-v*-Windows-x64.zip` (`MTF-Recomp-Setup.exe` + prebuilt `libmtfrt.a` toolchain), RecompLauncher Levels 1–3, in-game GitHub update notifier | Windows x64 only (no Linux/macOS binary release yet) |

---

## GPU & Platform Compatibility Notes

- **NVIDIA (Ada / Blackwell / Turing+):** Primary verified target. Full visual parity including ball trails, shadow cascades (`VK_NV_viewport_array2`), DLAA/DLSS, Optical-Flow Frame Generation, and HDR10.
- **AMD (RDNA 2 iGPU `24.30.66` tested):** Playable from menu into match with accurate 3D colors (`GeometryShaderPassthroughNV` -> standard Geometry Shader lowering) and zero Vulkan validation errors on boot. Known driver-specific visual limitation: A/B/Y shot ball trails render only their outer edges on `amdvlk64.dll 24.30.66`.
- **Intel:** Untested in `v0.1.6` (portable `GeometryShaderPassthroughNV` lowering and `VK_NV_*` guards apply automatically).
