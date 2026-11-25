# MTF Recomp — Performance & Memory Metrics Methodology

This document defines the standard protocol for measuring real gains in **stability**, **memory/VRAM usage**, and **CPU/GPU performance** across optimization stages (especially **Tier 2** in [roadmap.md](roadmap.md)).

---

## 1. A/B Comparison Rules

1. **Same hardware, same game build (`1.1.1`), and same scene:**
   - **Standard scenario:** 1v1 match on *Stadium Court* (or deterministic replay via `MTF_INPUT_SCRIPT`) for at least **1,800 frames (~30s)**.
2. **Two measurement profiles per stage:**
   - **Profile A — Real Gameplay (`60 Hz`):** `MTF_FPS_LIMIT=60`, `MTF_FRAMEGEN=off` — measures stutters, `1% low`, `0.1% low`, and allocation peaks.
   - **Profile B — CPU/Replay Ceiling (`Uncapped`):** `MTF_FPS_LIMIT=unlimited`, `VK_PRESENT=immediate`, `MTF_FRAMEGEN=off` — removes the 60 FPS cap to measure raw `replay_ms/frame` gains and maximum CPU/GPU FPS. *(Use only on a static scene/menu or short replay to measure backend cost, since simulation speeds up without Frame Gen.)*
3. **Environment variable toggle:** Every new optimization must expose a toggle (e.g., `MTF_VRAM_HEADROOM=0/1`, `MTF_HEAP_LAZY=0/1`, `MTF_REPLAY_THREADS=0/4`) for A/B comparison on the same machine and binary.

---

## 2. Collected Metrics

Extracted directly from [perfmon.c](../runtime/perfmon.c), [vk_memory.c](../runtime/vk_memory.c), and `build/fork/bin/logs/mtf.log`:

| Category | Metric | Runtime / Log Source | Goal |
|---|---|---|---|
| **Smoothness** | Average `FPS`, `1% low` (`low1_fps`), `0.1% low` (`low01_fps`) | `perfmon.c` / overlay (`F1`) | Eliminate drops and micro-stutters |
| **Vulkan CPU** | `replay_ms/frame`, `pipe_ms/frame`, `tex_ms/frame` | `perfmon.c` (`g_win`) | Reduce command buffer recording time |
| **Memory (RAM/Commit)** | `memory commit` (MB) at frame 1800 / peak | `mtf.log` (`mem: census` / `fatal` / `Win32`) | Reduce commit to fit comfortably in 16 GB RAM |
| **VRAM & Pools** | Imported pools (`MiB`), active mirrors, budget refusals | `mtf.log` (`mem: census at frame ...`) | Prevent OOM / thrashing on 6–8 GB VRAM GPUs |
| **Boot & Shaders** | `prewarm` time (ms) and `boot: guest starts` (ms) | `mtf.log` (`vk_pipeline: prewarm`) | Ensure no startup regression |

---

## 3. Evolution Table (Baseline → Tier 2 Stages)

> Fill in after each validated stage and record summarized logs in [devlog.md](devlog.md).

| Milestone | Commit | Memory Commit (MB) | Pools `mem: census` | `replay_ms/f` | `FPS` (Profile B) | `1% low` / `0.1% low` (Profile A) | Notes |
|---|---|---|---|---|---|---|---|
| **Baseline (pre-Tier 2)** | `a94cb62` | ~7,575–8,024 MB *(observed in earlier logs)* | `350 pools / 732.9 MiB` *(frame 1800)* | *TBD* | *TBD* | *TBD* | `MTF_PIPE_SYNC=1` default on NVIDIA |
| **Stage 2.1 (`vk_headroom` + `HEAP_LAZY` + `SPARSE_MIRROR_COPY` + commit)** | `debb4e7` | `5,076 MB` *(6s boot)* | `83 mirrors 436.8 MiB` + `1157 import 1592.4 MiB` *(frame 1800, user test; GPU ~4.7 GB VRAM incl. headroom)* | *TBD* | *TBD* | *TBD* | `budget 0` refusals; 7 pre-allocated 256 MiB blocks |
| **Stage 2.2 (`MTF_REPLAY_THREADS` + `DrawCtx`)** | `7210cac` | — | — | *TBD (`REPLAY_THREADS=0` vs `6`)* | *TBD* | *TBD* | `MTF_REPLAY_THREADS=6` active; full match validated by user |
| **Stage 2.3 (`spirv_nan_guard`)** | `13b296d` | — | — | — | — | — | `MTF_NAN_GUARD=0\|zero\|mark` available (`0` by default) |
| **v0.1.8 Sprint — NVIDIA High (RTX 5070 Ti, `878c190`)** | `878c190` | — | — | `async GPL, 0 pipe_waits>16ms` | — | `avg 60.0 fps / 1% low 52.6–55.6 / 0.1% low 34.2–51.3` | 4,800 frames (`mtf.log`); `boot: guest starts 801 ms`; `5` fast-links (avg `0.02 ms`, `0` failed) |
| **v0.1.8 Sprint — AMD iGPU Low (`24.30.66`, `878c190`)** | `878c190` | — | — | `prewarm 3.4s (was 21.7s)` | — | `avg 35.2–35.5 fps / 1% low 27.0–27.4 / 0.1% low 21.2–25.5` *(match)* | 2,400 frames (`mtf.1.log`); `boot: guest starts 4036 ms` (5.4x faster); `MTF_GPL_DRAW_LINK=0` added on AMD after `78.9 ms` frame 737 link |
| **T6 A/B (`MTF_REPLAY_ASYNC=0` vs `1`, AMD iGPU)** | `40be4f5` | — | — | `16.65 ms` vs `16.63 ms` | — | `60.1 fps / 1% low 58.0 / 0.1% low 43.0 vs 46.0` *(24s boot+menu)* | Negligible diff; `MTF_REPLAY_ASYNC=0` kept as default to preserve zero extra input latency |
