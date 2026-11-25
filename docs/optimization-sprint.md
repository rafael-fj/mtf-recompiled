# MTF Recomp — Optimization Sprint Plan (`v0.1.8`, branch `test`)

**Status:** planned (nothing below is marked done until its verification gate has been run and observed — see `AGENTS.md`).
**Branch rule:** every item is implemented and validated on `test` first; `main` only receives the merge after the Definition of Done below.
**Related:** [perf-methodology.md](perf-methodology.md) (A/B rules and metrics), [roadmap.md](roadmap.md), [port-status.md](port-status.md), [obstacles-and-quirks.md](obstacles-and-quirks.md).

---

## 1. Why this sprint exists

Two RecompLauncher reports for `v0.1.7` (Ryzen 7 7800X3D + RX 9070 XT, Ryzen 9 7900 + RTX 4080 SUPER) both reported **FPS drop spikes**, and a community request asked for "dynamic fps" so PCs below 60 FPS do not slow down.

### Evidence gathered (source in parentheses)

| Finding | Kind | Source |
| :--- | :--- | :--- |
| 198 draws waited for their pipeline, avg 36.4 ms, max 217 ms; 174 GPL fast-links over 5 ms on the drawing thread (max 185 ms, AMD LLPC driver) | measured | 9070 XT report `mtf.log` |
| First launch prewarmed only 260 pipelines; the NVIDIA report had `prewarm: no pipeline keys` and built pipelines synchronously (`0 fast links`) | measured | both reports |
| Local AMD iGPU prewarm of 1,737 pipelines delayed guest start by ~21.7 s | measured | local `build/fork/bin/logs/mtf.log` |
| Lifted game threads use ~0.61 busy cores at `-O2`: CPU of the lifted code is not the bottleneck | stated by repo | [CMakeLists.txt](../CMakeLists.txt) line 42 comment |
| Below 60 FPS the game slows down (fixed 1/60 s step per frame) | inferred | quirk #26 + user report |
| `par_threads()` returns 0 on NVIDIA only while `vk_pipeline_async_on()` is false | read from code | [vk_exec.c](../runtime/vk_exec.c) line 1234 |

### Already on `test` before this sprint (built and boot-smoke-tested only; **no gain measured**)

| Commit | Change |
| :--- | :--- |
| `06b64c9` | Per-frame one-shot pipeline wait budget (`MTF_ONESHOT_BUDGET_MS`, default 16), GPL workers up to 8 (`cpus/2`), async GPL + multi-thread prewarm on NVIDIA when `vk_caps.gpl` |
| `6577b8a` | RecompLauncher graphics settings (SMAA, ANISO, SHADOW_SCALE, SHARPNESS, HDR), new defaults, SMAA skipped when DLAA/DLSS is active |
| `e277f45` | `Recommended` hardware-aware preset; launcher `fps_limit` limited to 30/60 |
| `22edab2`, `e50df1d`, `9e01b8f` | `ViewportMaskNV` → `MultiViewport` shadow cascades on AMD/Intel, buffer-size crash fix, gated to `SHADOW_SCALE >= 2` |

**Known gaps in that work (to be closed by this sprint):**
1. No before/after measurement exists for `06b64c9`.
2. `06b64c9` implicitly turned on slice-parallel replay recording by default on NVIDIA with >= 8 CPUs (see table above). Only a boot smoke test was run; character select and a match were not tested on NVIDIA with it.
3. The `Recommended` preset was only exercised on the `integrated-low` tier. The `rtx-dlaa`, `discrete-high` and `discrete-mid` tiers were not run.
4. iGPU FPS after the `SHADOW_SCALE >= 2` gate has not been re-measured. The 8-worker pool (was 4) competing with game threads on a low-core machine is also a suspect.

---

## 2. Principles

- One toggle per optimization (`MTF_*`), so the same binary supports A/B (per [perf-methodology.md](perf-methodology.md) rule 3).
- Use the existing measurement tools: `perfmon.c` (`low1_fps`, `low01_fps`, `replay_ms`, `pipe_ms`), `MTF_INPUT_SCRIPT` (scripted input) and `mtf.log`.
- Profile A (`60 Hz`, `MTF_FRAMEGEN=off`) for smoothness; Profile B (uncapped) only on static scenes.
- Targets below are **proposals**; they are calibrated after T0 produces a baseline.

---

## 3. Work items

Order: T0 → T1 → T2 → (T3, T4 in parallel) → T5 spike → T5/T6 → T7.

### T0 — Baseline *(blocking)*
- Capture the standard scenario (1v1 Stadium Court, >= 1,800 frames, `MTF_INPUT_SCRIPT`) for `main` (`v0.1.7`) and `test` on: RTX (NVIDIA), AMD integrated GPU, and the 9070 XT if a tester is available.
- Record `1% low`, `0.1% low`, `replay_ms/f`, `pipe_ms/f`, `prewarm` ms and `boot: guest starts` ms in the evolution table of [perf-methodology.md](perf-methodology.md).
- **Gate:** table filled for at least NVIDIA + AMD iGPU, first run (no `pipeline-keys.bin`) and second run.

### T1 — Frame-time telemetry in `mtf.log`
- `perfmon.c` already computes `low1_fps` / `low01_fps` for the overlay, but nothing writes them to `mtf.log`, so RecompLauncher reports carry no frame-time data.
- Log a periodic summary (average, 1% low, 0.1% low, `pipe_ms/f`, `replay_ms/f`, count of pipeline waits over 16 ms) and a final summary at exit.
- Toggle: `MTF_PERF_LOG` (default on, low overhead).
- **Gate:** summary lines appear in `mtf.log` for a 60 s run; overhead below measurement noise in A/B.

### T2 — Validate and decouple NVIDIA slice threads
- Decouple `par_threads()` from `vk_pipeline_async_on()` so enabling async GPL does not silently change the recording mode.
- A/B `MTF_REPLAY_THREADS=0` vs default on NVIDIA through character select (>= 256 draws, the case in quirk #29) and a full match; decide the default from data.
- **Gate:** no crash/validation error (`MTF_VK_VALIDATION=1`) in character select + a match with both settings; decision recorded in the devlog.

### T3 — Non-blocking prewarm
- Today the prewarm blocks guest start (21.7 s on the local iGPU).
- Add a blocking budget (`MTF_PIPE_PREWARM_BLOCK_MS`, proposed default a few seconds); the remaining records keep building on the workers after the guest starts, ordered so menu/court pipelines come first.
- Draws whose pipeline is not ready are already skipped, so correctness risk is low; verify no black frames in menu → match.
- **Gate:** `boot: guest starts` on the iGPU drops versus the T0 baseline; second-run spike count not worse than before.

### T4 — Pipeline spike reduction
- Move GPL fast-link work off the drawing thread when it exceeds the 5 ms threshold (AMD driver case).
- A/B pipeline workers (4 vs `cpus/2` up to 8) on a low-core machine and on the iGPU.
- Tune `MTF_ONESHOT_BUDGET_MS` (8 / 16 / 33) and review whether depth-only/LUT passes and icon captures should have different budgets.
- **Gate:** count and max of `a draw waited ... ms for its pipeline` lines in the first match improves versus T0 on the AMD report scenario.
- **Open decision:** seeding a first-run `pipeline-keys.bin` would help first sessions but contains hashes derived from game shaders; check it against the content boundary in `AGENTS.md` / `tools/check_content.py` before any shipping.

### T5 — Dynamic resolution (answer to the "dynamic fps" request)
- Instead of changing the game's fixed timestep (reverse engineering of `nro0`), lower `RENDER_SCALE` live when GPU frame time exceeds the 60 Hz budget, with hysteresis and a floor.
- **T5a (spike first):** `RENDER_SCALE` is a `RESTART` setting today (`cfg.c` meta). Determine whether scene targets can be resized live without a pipeline/target rebuild hitch; abandon or narrow the scope if not.
- Constraints: disabled when HDR is active (the menu locks `Render Scale 100%`), when DLSS/DLAA is active, or when the user pinned a render scale.
- Toggle: `MTF_DRS` (default off until measured). Exposed in `F1` and `recomplauncher.json` only after the gate.
- **Gate:** on the AMD iGPU, 1% low improves and the game does not run in slow motion in the T0 scenario.

### T6 — Experiment: `MTF_REPLAY_ASYNC`
- A/B only (it costs up to one frame of latency per [vk_backend.c](../runtime/vk_backend.c) header). No default change without T1 data.

### T7 — Wrap-up
- Update `docs/devlog.md`, `docs/port-status.md`, `docs/status-details.md`, `docs/roadmap.md`, `docs/perf-methodology.md`; README only if defaults visible to users change.
- Run `python tools/build_fork.py` and `python tools/check_content.py`; merge `test` → `main`; release notes for `v0.1.8`.

---

## 4. Explicitly out of scope

- Reverse engineering the `nro0` timestep for native > 60 FPS / true Dynamic FPS (tracked separately; `fps_limit` stays at 30/60 in the launcher until then).
- Re-enabling the recorder thread and slice workers on AMD (disabled for historical device-lost/hang reports, see the comment in [vk_rec.c](../runtime/vk_rec.c)); needs its own test bench.
- PGO/LTO of lifted modules (low expected return given the 0.61-core figure).
- AMD RDNA 2 A/B/Y ball-trail interior.

---

## 5. Definition of Done

1. T0–T4 and T7 gates passed with observed results recorded in `devlog.md` and the evolution table; T5/T6 either passed or documented as rejected with data.
2. No regression in boot, menu → character select → match on NVIDIA and AMD iGPU (with `MTF_VK_VALIDATION=1` smoke).
3. `check_content.py` clean, `build_fork.py` clean.
4. Every optimization has a documented `MTF_*` toggle.

## 6. Risks

| Risk | Mitigation |
| :--- | :--- |
| Parallel recording on NVIDIA regresses (quirk #29 class) | T2 before anything else touches threading |
| Non-blocking prewarm shows missing passes early in a session | Priority ordering; verify menu → match; toggle to restore blocking |
| DRS needs live target resizing | T5a spike decides feasibility first |
| Limited test hardware (NVIDIA RTX dev PC, AMD iGPU) | Ask RecompLauncher testers (9070 XT, 4080 SUPER) to run the T1 logging build |
