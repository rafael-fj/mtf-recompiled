# MTF Recomp — Port Status

**Last updated:** 2026-11-28  
**Target game:** *Mario Tennis Fever* (Switch 2 / `Ounce`, Title ID `0400D4201C6EA000`, Update `1.1.1` / `v327680`)  
**Current phase:** `v1.0.1` + QA/Perf/Streaming Hardening — Deterministic 8,538-poll replay verified (`0` Vulkan validation errors, `0` UBSan runtime errors, `58.21 avg fps` / `-73%` frames >20 ms via write-watch pre-snap coalescing), plus WebRTC/WebSocket/AV1 multiplayer fixes. See [status-details.md](status-details.md) for the full percentage breakdown.

## Module Status

| Module | File Size | Disk MD5 / SHA-256 | Lifted | Compiled | Linked | Verified |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `main` (`nxboot`) | 14,069,153 B | MD5 `115e98ed471509dd30785b35cf8a0d58` | **1,589 fns** (10 MB C) | **Yes** (4 chunks) | **Yes** (`mtf-recomp.exe`) | Boots & unpacks `nro0` |
| `sdk` (20.5.17) | 7,326,594 B | MD5 `17fd7a3ffd8eb8af7500310841d4cf54` | **47,907 fns** (138 MB C) | **Yes** (50 chunks) | **Yes** (`mtf-recomp.exe`) | Boots & initializes HLE |
| `nro0` (runtime-unpacked) | 24,764,416 B | SHA-256 `1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975` | **60,968 fns** (336 MB C) | **Yes** (151 chunks) | **Yes** (`mtf-recomp.exe`) | 60 Hz gameplay + REV15 audio + VP9 |

## Subsystem Status

| Subsystem | Status | Notes |
| :--- | :--- | :--- |
| Dump Validation & Content Guard | **Ready** | `tools/check_dump.py` & `tools/check_content.py` passing (CI enforced) |
| ARM64 Lifter (`recomp/`) | **Ready** | 0 unsupported on `main`+`sdk`, 1 `ldst` on `nro0`; 119,955 Unicorn diff cases passed |
| Binary Index (`tools/analisis/bindb.py`) | **Ready** | `build/bindb.sqlite` indexes 98,560 fns, 29,422 syms, 573,750 calls, 54,969 strings |
| Horizon Kernel & `ro::LoadModule` | **Ready** | Boots `main` + `sdk` + `nro0` cleanly to gameplay |
| NVN2 Front-End & Vulkan Backend | **Playable (NV 96% / AMD 86%)** | Vulkan 1.3 renderer, Gate F5 (`0` validation errors on NVIDIA), async GPL on NVIDIA (`8` workers), non-blocking boot prewarm (`MTF_PIPE_PREWARM_BLOCK_MS=3000`), per-frame `one-shot` budget (`16 ms`), `GeometryShaderPassthroughNV` & `ViewportMaskNV` (`MultiViewport`, `SHADOW_SCALE >= 2`) lowering, hardware-aware `Recommended` preset, `MTF_DYNAMIC_FPS` `< 60 FPS` 60-tick accumulator (verified playable on `AMD Radeon(TM) Graphics` `0x1002:0x13c0` iGPU at `low` preset, `avg 42.6–45.1 fps` / `60 ticks/s`), HDR10, Frame Gen, FSR 1; A/B/Y ball-trail interior still missing on AMD RDNA 2 (`24.30.66`) |
| Shader Pipeline (`CRA.` → SPIR-V) | **Ready (F4+F5)** | **35,045 / 35,045** shaders translated to SPIR-V (0 failed); `ensure_storage_caps` injects `StorageImageRead/WriteWithoutFormat`; `nvn2_offline_get` loads offline `.spv` + `.res.json` |
| Audio (`audren` REV15) & Video (`ov_codec`) | **Ready (F3)** | REV15 `audren` + SDL3 (`48000 Hz x240`, 96 voices) & `libvpx` VP9 (`2560x1440`) active |
| Multiplayer & Browser Streaming (`F1 -> Multiplayer`) | **Ready** | Local `P1..P4` input routing + WebRTC UDP DataChannel / Cloudflare Tunnel / Local Host browser streaming (`AV1` on Chromium / `H.264` on Firefox + `Opus`), automatic WebSocket fallback, `Performance`/`Balanced`/`Quality` presets, 3s player connect/disconnect toasts |
| Installer, RecompLauncher & Updates | **Ready** | GitHub Actions `--onedir` release build, `recomplauncher.json` (Levels 1–3) + `--prepare`, in-game GitHub release notifier (`UPDATE_NOTIFIED_TAG` + `F1` reminder) |
