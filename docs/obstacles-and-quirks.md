# MTF Recomp — Known Obstacles & Game-Specific Quirks

This catalog tracks every *Mario Tennis Fever*-specific technical obstacle using a strict 4-part format: **Symptom**, **Root Cause**, **Solution**, and **Verification**.

---

## 1. Runtime-Unpacked Game Code (`nxboot` $\rightarrow$ `nro0`)
- **Symptom:** `exefs/main` has only ~600 KB of `.text` alongside ~13.4 MB of high-entropy `.data`; static analysis of `main` alone reveals no gameplay logic or shader tables.
- **Root Cause:** `main` is a boot stub (`nxboot`) that spawns 6 worker threads to unscramble/unpack the real 24.7 MB `NRO0` (`nro0`) out of `.data`, registers `romfs/.nrr/nrr.nrr` (NRR v1), and calls `nn::ro::LoadModule`.
- **Solution (F2):** Implement `MTF_NRO_CAPTURE` in `runtime/ro.c` to capture the unpacked `nro0` during a 2-second stage-1 boot of `main` + `sdk`, verify its SHA-256 (`1ec347bc9f4664b467fc5f9b3a9336fe91e22e119fc7fb60f8cf44f0d30ce975`) against `nrr.nrr`, and lift/compile `nro0` as a first-class module.
- **Verification:** `nro0` SHA-256 matches `nrr.nrr` and `nn::ro::LoadModule` binds to the compiled native `nro0` functions.

## 2. `CentralHeap::Init` Abort at Boot (`mem_alias` Size)
- **Symptom:** `CentralHeap::Init` aborts at `sdk+0x13f698` immediately during `nninitStartup`.
- **Root Cause:** `nninitStartup` requests 16 GB of address space from `StandardAllocator` while SDK 20.5.17 retains a 1 GB node area; a 16 GB `mem_alias` region is too small.
- **Solution (F2):** Reserve `mem_alias = 0x500000000` (20 GB) and `mem_reserve = 0x650000000` (25.25 GB) in `runtime/memory.c` (per `profiles/0400D4201C6EA000/profile.json`).
- **Verification:** Game completes `nninitStartup` and reaches `nnMain`.

## 3. Boot-Stub Thread Stack-Slot Reuse & Async Preemption Race
- **Symptom:** Intermittent `0xc0000005` crash (`addr=0xffffffffffffffff`) inside `main`'s 6-thread `nro0` unpacker (`f_7000001d80`, `exe+0x6dc4`) or `f_7000001400` (`exe+0x54a4`) when `TOTK_ASYNC_PREEMPT=1` is active.
- **Root Cause:** Asynchronous virtual-core preemption (`kernel.c`) interrupts `main`'s 6 tight MT19937 unpacker threads on Switch 2 cores `0..5` (additionally, `kernel.c` `CreateThread` initially rejected `core > 2` with `0xfa01` until updated to `core >= NCORE`).
- **Solution (F2):** Update `kernel.c` `CreateThread` (`svc #0x08`) to accept `0 <= core < NCORE` (`NCORE=6`, mask `0x3f`); run Stage-2 boot with `TOTK_ASYNC_PREEMPT=0` while investigating safe preemption points for `main`'s unpacker threads.
- **Verification:** `MTF_GPU=null TOTK_ASYNC_PREEMPT=0 mtf-stage2.exe --boot` deterministically unpacks `nro0`, runs `nro0 DT_INIT`, and reaches `nvnDeviceGetProcAddress`.

## 3a. `nn::fs::SetAllocator` Calling `fsp-srv` & `nro0` `R_AARCH64_TLSDESC` Relocations
- **Symptom:** (a) `nn::fs::SetAllocator` aborts on unimplemented `fsp-srv` via `nn::fs::detail::IsEnabledAccessLog`; (b) `loader.c` aborts on `nro0 relocation 0 has type 1031`.
- **Root Cause:** (a) SDK 20.5.17's `SetAllocator` queries `IsEnabledAccessLog` over `fsp-srv`; (b) `nro0` uses 10 `R_AARCH64_TLSDESC` (`1031`) relocations (`sym=0`, offsets `0x0..0x50`).
- **Solution (F2):** Override `nn::fs::SetAllocator` / `IsEnabledAccessLog` in `runtime/fs_host.c`, and add `tlsdesc_resolve` (`0x1000 + addend` inside each thread's `0x2000`-byte `TLS_SIZE` block) in `runtime/loader.c`.
- **Verification:** `nro0` loads cleanly with 82,193 relocations and 0 unresolved strong imports.

## 4. `nn::fontll` 50 KB Work Buffer Crash (`nro0+0xaef8a8`)
- **Symptom:** `ScalableFontEngine::LoadFont` returns `201`, `SetFont` returns `301`, and the game crashes on a null pointer at `nro0+0xaef8a8` looking up `"?"`.
- **Root Cause:** MTF gives its second font engine only 50 KB (`0xc800`) of work memory; full hinted system/fallback fonts (e.g., Arial Unicode) require 56–60 KB.
- **Solution (F2):** Serve stripped *unhinted* fallback fonts (24–32 KB footprint) via `runtime/ov_font.c` or user-supplied Switch firmware fonts.
- **Verification:** `LoadFont` returns `0` on both font engines.

## 5. `CRA.` Single-File Scrambled RomFS (`rom:/42`)
- **Symptom:** RomFS contains a single 8.17 GB file named `42` (`CRA.` magic) with no embedded directory tree.
- **Root Cause:** Camelot's `nxe` engine stores the 8,671-entry file table, checksums, and suffix list inside `nro0`'s `.rodata`, scrambling 16-byte blocks with `crc32x`/`crc32cx` keys derived from `"UseUrSk1lls4G00d"`, `"Nintendo/CAMELOT"`, and the constant offset `0xa756e6f0` (`_X10_XOR = 0x02052636 ^ 0xA756E6F0`).
- **Solution (F4):** Implemented `tools/shaders/cra.py` + `tools/shaders/cra_fast.c` and `tools/shaders/extract.py` to decrypt `rom:/42` and unpack `MMMM`/`NARC`/`SARC`/`BNSH` shaders for offline SPIR-V translation, while `runtime/romfs.c` serves `rom:/42` directly to the game's own reader.
- **Verification:** `cra.py --verify` passed with 0 errors, and `extract.py` extracted all 35,045 unique shaders (`50,287` `vs`/`ps` pairs) from `rom:/42` + `nro0`.

## 5a. SPIR-V `GenerateCall` `InvalidCastException` / `KeyNotFoundException` on `sm86` Shaders
- **Symptom:** 7/200 shaders in the initial `translate.py --limit 200` batch failed in `Ryujinx.Graphics.Shader.CodeGen.Spirv.Instructions.GenerateCall` with `InvalidCastException` (e.g., `pfx.narc`) or `KeyNotFoundException` (e.g., `sdf_font_compute.narc`).
- **Root Cause:** `AstOptimizer` / `Simplification` can pass non-`LocalVariable` operands (constants, `AstOperation` expressions, or unregistered locals) as arguments to `Instruction.Call`, whereas SPIR-V `OpFunctionCall` requires pointer operands (`Function`-storage-class `OpVariable`).
- **Solution (F4):** Added `CodeGenContext.TryGetLocalPointer` and updated `Instructions.GenerateCall` to allocate a temporary `Function`-storage-class `OpVariable` (`context.AddLocalVariable`) and store `context.Get(argType, operand)` into it whenever an argument is not a registered `LocalVariable`.
- **Verification:** `python tools/shaders/translate.py` translated all **35,045 / 35,045** shaders in `53.6s` with **0 failures**.

## 6. VP9 1440p@60fps Opening Movie Black Frames & Choppy Audio (`nn::codec`)
- **Symptom:** Title movie (368 MB VP9 2560x1440@60fps + Vorbis) shows black/corrupt frames or 60–80 ms audio gaps every 1–2 seconds.
- **Root Cause:** (a) MTF's player (`nro0+0x3b65fc`) sends one packet per submission-ready signal and ignores refusal; (b) it shares one source clock for video and audio, pausing audio whenever VP9 decoding falls behind.
- **Solution (F3):** In `runtime/ov_codec.c`, track queued + held + decoding packets together (`CV_HOLD=12`) for event signaling, decode on a dedicated host thread, and configure `libvpx` with 4 threads and `VP9D_SET_ROW_MT=1`.
- **Verification:** Title movie plays smoothly at 60 FPS with 0 audio underruns/gaps.

## 7. Blue Wash Over 3D Scenes (Passthrough Geometry Shader)
- **Symptom:** Every 3D scene with atmosphere has a solid blue tint.
- **Root Cause:** The `8x8x64` 3D atmosphere LUT is populated once per stage using a Maxwell/Ampere passthrough geometry shader (header bit 24) that writes `gl_Layer`; if the translator assumes native passthrough support without emitting primitives/layer, the LUT fails to populate.
- **Solution (F4):** Configure the shader translator (`tools/xlate/`) to lower passthrough geometry shaders portably (emitting input primitives and `gl_Layer`), or map to `VK_NV_geometry_shader_passthrough` where supported.
- **Verification:** Stadium Court atmosphere renders with natural sky/lighting colors.

## 8. Null Vertex Buffers Bound Past Stream 15 & `nvnWindowSetCrop`
- **Symptom:** Excess validation warnings / crashes on streams `16..31`, and black bands on the right/bottom of screenshots.
- **Root Cause:** MTF clears 32 vertex streams (`16..31` with null ranges) before every model, and renders at `2048x1152` inside a `2560x1440` window texture cropped via `nvnWindowSetCrop`.
- **Solution (F5):** Ignore vertex stream binds with `first + count > 16` cleanly in `nvn_api_cmd.c`, and honor `nvnWindowSetCrop` in the Vulkan swapchain blit.
- **Verification:** 0 Vulkan validation errors during gameplay; clean full-frame presentation.

### Q8: `nn::hid::GetNpadState` (singular) vs `GetNpadStates` (plural) in MTF `nro0`
- **Symptom:** `sdk+0x64d420` aborted with `Result 0x58cca` even though `GetNpadStates` was overridden.
- **Root cause:** MTF's `nro0` imports `nn::hid::GetNpadState` (singular, 2 args) in addition to `GetNpadStates` (plural, 3 args), plus `SetNpadHandheldActivationMode`, `GetNpadControllerColor`, and `SetGyroscopeZeroDriftMode`.
- **Fix:** Extracted `fill_npad_state()` in `runtime/ov_input.c` and registered `HOST_OVERRIDE`s for all singular `GetNpadState` overloads and the remaining `nn::hid` symbols.
- **Verification:** `mtf-stage2.exe --boot` initializes all HID controllers, six-axis sensors, and vibration devices without hitting `sm: service "hid"`.

### Q9: `nn::oe` & `nn::friends` system events + missing shared fonts
- **Symptom:** After overriding `nn::oe::Initialize` and `nn::friends::Initialize`, `GetDefaultDisplayResolutionChangeEvent`, `GetNotificationMessageEvent`, and `GetFriendInvitationNotificationEvent` returned uninitialized `SystemEvent` pointers or hit `friend:u`, and `nn::pl::GetSharedFontSize(0)` aborted when Switch firmware `.bfttf` files were not provided.
- **Root cause:** MTF polls `nn::oe::GetNotificationMessageEvent` via `TimedWaitSystemEvent` and initializes `nn::friends` and `nn::fontll` during boot.
- **Fix:** Added `nn::friends` offline overrides and `nn::oe` event/resolution overrides in `runtime/ov_system.c` using `kern_bind_system_event`, plus a `C:/Windows/Fonts/arial.ttf` fallback when `.bfttf` shared fonts are absent.
- **Verification:** `mtf-stage2.exe --boot` passes `nn::friends`, `nn::oe`, and `nn::fontll` initialization cleanly.

### Q10: NVN2 `BindVertexBuffers` slots `16..31`
- **Symptom:** `nvnCommandBufferBindVertexBuffers` logs `nvn: BindVertexBuffers first 16..31 n 1` during rendering.
- **Root cause:** Switch 2 NVN2 exposes 32 vertex buffer binding slots (`0..31`), while `vk_pipeline` / `nvn_api_cmd.c` caps vertex buffer bindings at `0..15`.
- **Fix:** Silently ignore `first >= 16 && n >= 0 && first + n <= 32` in `runtime/nvn_api_cmd.c` (`bind_vertex_buffers`).
- **Verification:** Confirmed zero `BindVertexBuffers` log spam during Vulkan boot.

### Q11: `nvgpucomp64.dll+0x1909f2` Crash on `OpFunctionCall` Pointer Type Mismatch & Mid-Function `OpVariable`
- **Symptom:** `vkCreateComputePipelines` crashed inside `nvgpucomp64.dll+0x1909f2` (`addr=0x10`) on compute shader `ec0cf68e68e5f636` (and `68c74a61e9b34c61`), even though `vkCreateShaderModule` succeeded.
- **Root cause:** (a) `Spv.Generator.Module.AddLocalVariable` appended `OpVariable`s to the current block when called mid-function by `GenerateCall`; (b) `Instructions.GenerateCall` passed `LocalVariable` pointers directly even when `local.VarType != function.GetArgumentType(i)` (e.g., passing `float*` to an `int*` parameter, affecting 433 shaders).
- **Fix:** Updated `Module.AddLocalVariable` to insert at the end of the function's entry-block `OpVariable` list, and updated `Instructions.GenerateCall` to materialize a `Function`-scope `tempVar` of `argType` with bitcast copy-in (`i < InArguments.Length`) / copy-out (`i >= InArguments.Length`) whenever `local.VarType != argType`. Re-translated all 35,045 shaders.
- **Verification:** `ec0cf68e68e5f636` and all subsequent compute/graphics pipelines compile cleanly on RTX 5070 Ti.

### Q12: Unused `OpDecorate Binding` (`cb0`) Omitted from `.res.json` Reflection
- **Symptom:** `vk_pipeline: graphics pipeline not built: the vertex shader uses binding 1, which its reflection and layout lack` on `c674dc35b7a82578` and `2da4ab693163e08e`.
- **Root cause:** `Ryujinx.Graphics.Shader` can declare an unused `cb0` (`OpVariable` + `OpDecorate Binding 1`) without adding it to `CBuffers`, causing `vk_pipeline.c` (`module_binding_missing`) to reject the pipeline.
- **Fix:** In `runtime/gfx_shader.c` (`nvn2_offline_get`), scan the loaded SPIR-V for `OpDecorate ... Binding >= 1` and append any missing binding to `s->res[GFX_RES_UBO]`.
- **Verification:** Graphics pipelines for `c674dc35b7a82578` and `2da4ab693163e08e` build cleanly with 0 skipped draws.

### Q13: Black screen and no audio after boot (`StartVp9Decoder` ready-event race)
- **Symptom:** `mtf-stage2.exe --boot` opened the Vulkan window and ran the main loop, but the screen stayed black with no audio output.
- **Root cause:** `nro0` uses `main`'s `movie::VideoDecoder` (`nn::codec::Vp9Decoder`). `ov_vp9_ready_event` and `ov_vp9_acquire` signalled the submission-ready `SystemEvent` before `StartVp9Decoder` ran, while `StartVp9Decoder` was bound to `ov_ok` (no-op). `MovieEventHandler` (`nro0+0xd16460`) woke up early, called `ClearSystemEvent`, and posted an input-ready message to `MovieVideoHandler` (`nro0+0xd19c00`), which dropped it because `Start` had not yet set its active flag — leaving the decoder stalled on `RequestSyncPointEvent(0xc0de0000)`.
- **Fix:** Bound `_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE` in `runtime/ov_codec.c` to `ov_vp9_start`, which signals the submission-ready `SystemEvent` via `kern_signal_system_event(event_for(c, key))`.
- **Verification:** `mtf-stage2.exe --boot` decodes 2560x1440 VP9 video (`pass_0004_t0_2560x1440_43_rgba8srgb.png` at frame 120) and outputs non-zero audio (`first nonzero output after 182 frames`, 0 underruns).

### Q14: Crash in `nn::account::LoadProfileImage` (`0xc0000005` at `sdk+0x184f20`)
- **Symptom:** After advancing past the initial boot/movie or interacting, `mtf-stage2.exe` crashed with `0xc0000005` at offset `0x71b59b` (`f_7000f84f20`, `nn::account::LoadProfileImage`).
- **Root cause:** `nn::account::LoadProfileImage` was not overridden in `runtime/ov_system.c`, so it executed original `sdk` code calling `f_700159a2b0` (unimplemented `acc:u` IPC) which returned `NULL`, causing a null-vtable dereference.
- **Fix:** Added `ov_load_profile_image` in `runtime/ov_system.c` returning a valid 64x64 baseline JPEG (`693` bytes) for the local user UID.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.7s`), ran `TOTK_TRACE=perf,audio` cleanly at `60.0 fps` with `0` audio underruns/overruns.

### Q15: `FATAL: nvn: CallCommands with a null handle` on Title/Menu Transition
- **Symptom:** Pressing `E` (`B_A`) on the title/menu screen crashed with `FATAL: nvn: CallCommands with a null handle`.
- **Root cause:** `runtime/nvn_api_cmd.c` (`inline_recording`) aborted via `rt_fatal` when the game passed a `0` command handle to `nvnCommandBufferCallCommands`.
- **Fix:** Updated `inline_recording` in `runtime/nvn_api_cmd.c` to log once (`nvn: CallCommands: ignoring null handle`) and return early when `!handle`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.7s`); confirmed in `work/last_launch.log` that `CallCommands` with a null handle is now logged and survived cleanly.

### Q16: `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented` on Menu Transition
- **Symptom:** After surviving `CallCommands` with a null handle upon pressing `E` (`B_A`), `mtf-stage2.exe` aborted with `FATAL: NVN nvnCommandBufferSetViewportSwizzles is not implemented`.
- **Root cause:** `nvn_api_core.c` reported `NVN_DEVICE_INFO_SUPPORTS_VIEWPORT_SWIZZLE = 1` (`devinfo[0x43]`), and `nvnCommandBufferSetViewportSwizzles` was still an unimplemented stub in `nvn_unimpl.inc`.
- **Fix:** Implemented `NVN_API(nvnCommandBufferSetViewportSwizzles)` in `runtime/nvn_api_cmd.c` (validates identity swizzles `(POSITIVE_X, POSITIVE_Y, POSITIVE_Z, POSITIVE_W)` and logs once if any non-identity swizzle is requested). Also wired `TOTK_DUMP=key` (`TOTK_DUMP_DIR=work/f9_dump`) into `installer/mtf_setup.py` `on_launch()` so `F9` frame dumps work out of the box.
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.8s`), verified `60.0 fps` (`120 frames in 2.000 s`) and `python tools/check_content.py` (`0 violations`).

### Q17: Indexed `NVN_DRAW_PRIMITIVE_QUADS` Missing Half of Every Quad (6x4 Black Triangle Grid)
- **Symptom:** The post-title 3D tennis court (`pass 0010`, `24` tiled deferred-lighting draws) and other quad-based draws rendered only one triangle per quad, leaving a `6x4` grid of dark/black triangles across the screen.
- **Root cause:** `runtime/vk_exec_draw.c` expanded `NVN_DRAW_PRIMITIVE_QUADS` to `{0, 1, 2, 0, 2, 3}` in `op_draw` (`DrawArrays`), but `op_draw_idx` (`DrawElements`) left `NVN_DRAW_PRIMITIVE_QUADS` unexpanded while `vk_pipeline_topology` mapped it to `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`. With `d->count == 4`, Vulkan drew only the first 3 indices (1 triangle) and dropped the 4th vertex.
- **Fix:** Added `NVN_DRAW_PRIMITIVE_QUADS` index expansion (`{i0, i1, i2, i0, i2, i3}` for `q = count / 4` quads, supporting `UNSIGNED_BYTE`, `UNSIGNED_SHORT`, and `UNSIGNED_INT` host-readable index buffers) in `op_draw_idx` (`runtime/vk_exec_draw.c`).
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.4s`) and dumped frame `600` (`work/dump_post_fixed/pass_0010_t0_1706x960_122_r11g11b10f_tm.png`): all 24 deferred-lighting tiles render completely with zero black triangles.

### Q18: VP9 Intro Movie Running at 20 FPS with Stuttering Audio (`ov_vp9_submit` Submission-Ready Event)
- **Symptom:** Although the render loop presented at 60 FPS, the `2560x1440` VP9 intro movie decoded only 1 video frame every ~50 ms (3 VSyncs = ~20 FPS, reaching `pts 1.70 s` in ~6 s) and audio was mostly silence (`53,493 / 239,520` nonzero callback frames over 5 s).
- **Root Cause:** In `runtime/ov_codec.c`, `ov_vp9_submit` decoded into the oldest `S_WAIT` buffer without re-signalling the `GetVp9DecoderSubmissionReadyEvent` when additional `S_WAIT` buffers were still available (`oldest_wait(d) != NULL`). The player cleared the event after submitting and therefore slept until `ReleaseVp9DecoderOutputBuffer` + `AcquireVp9DecoderOutputBuffer` ran ~50 ms later. Additionally, signalling when `oldest_wait(d) == NULL` caused the demuxer to fill the 8-entry queue and drop packets on `R_NOT_READY`, corrupting VP9 reference frames.
- **Fix:** Updated `ov_vp9_submit` in `runtime/ov_codec.c` to signal `submission-ready` when `oldest_wait(d) != NULL` and clear it (`clear_event`) when no `S_WAIT` buffer remains, and bumped `libvpx` decoder threads to `4` for `>= 1920` width (`vp9_init`).
- **Verification:** Rebuilt `mtf-stage2.exe` (`0.5s`) and ran `TOTK_TRACE=perf,codec,audio` for 8 s: zero `not ready` or VP9 decode errors, `60.0 fps` (`120 frames in 2.000 s`), `0` audio underruns/overruns/skipped, `182,982 / 239,520` nonzero audio frames, and movie PTS reached `5.07 s` smoothly.

### Q19: Missing UI Font Text & Wrong NVN2 Compute `LocalSize` (`ctl+0x58` & `R32_UINT` Storage Image View)
- **Symptom:** Title screen rendered only the light-blue background and black bottom bar (`distinct=218`) with all text (`Press ? + ?`, copyright, version) missing; post-title tutorial UI text was also invisible.
- **Root Cause:** Two independent bugs prevented the `sdf_font_compute.narc` compute shaders (`cs_font_0`, `cs_font_1`) and all other NVN2 compute shaders (`skinning.narc`, `blendshape_compute.narc`) from producing valid output:
  1. **NVN2 compute `LocalSize` offset:** NVN2 (`0x98761234`, `0xc9`) compute shader control headers (`0xd8` bytes) store `local_x, local_y, local_z` at **`ctl + 0x58`** (`16x16x1` for font compute), not at NVN1's `ctl + 0x748`. Reading out-of-bounds at `+0x748` produced `LocalSize(1, 1, 1)` in all 78 translated NVN2 compute shaders and wrong workgroup sizing in runtime reflection.
  2. **`R32_UINT` storage image reinterpretation:** `cs_font_0` and `cs_font_1` write/read an intermediate `256x256` `NVN_FORMAT_R32UI` (`VK_FORMAT_R32_UINT`) storage image via SPIR-V `OpTypeImage(..., OpTypeFloat 32, ImageFormat = Unknown)` + `OpBitcast` (`floatBitsToUint`), which requires a `VK_FORMAT_R32_SFLOAT` image view on Vulkan.
- **Fix:**
  1. Updated `tools/shaders/translate.py` (when `"local"` is absent from `.entry`), `runtime/gfx_shader.c`, and `runtime/vk_exec_draw.c` to read NVN2 compute `LocalSize` from `ctl + 0x58`, and re-translated all 78 NVN2 `cs` shaders (`0 failed`).
  2. Updated `get_view` in `runtime/vk_exec_draw.c` to reinterpret `VK_FORMAT_R32_UINT` / `VK_FORMAT_R32_SINT` as `VK_FORMAT_R32_SFLOAT` when `storage && !rfmt`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.5s`), dumped frame `320` (`work/dump_title_text_fix2/pass_0020_t0_2560x1440_43_rgba8srgb.png`, `distinct` `218` $\rightarrow$ `879`, crisp `Press ? + ?`, `© Nintendo / CAMELOT`, `Ver. 1.1.1`) and frame `600` (`work/dump_post_chars_fix/pass_0038_t0_2560x1440_43_rgba8srgb.png`, tutorial prompt `Let's go over some basic button controls before jumping into the game.` and `?/? End practice` rendered cleanly).

### Q20: Solid Light-Blue Title Screen & Missing UI Boxes (`NxUniformSizesBank` / `slot 25`)
- **Symptom:** The title screen (`frame 320`, `pass 0020`, `pipe 0`) rendered a solid light-blue rectangle (`170, 200, 216`) instead of the `2560x1440` `BC7_SRGB` Mario Tennis Fever key art (`tex 489`), and the post-title tutorial dialog box (`frame 600`, `pass 0038`) was missing behind its text.
- **Root Cause:** `totk_xlate` compiles shaders with `TranslationFlags.MetalUniformBounds`, which emits dynamic UBO bounds checks against `NxUniformSizesBank` (constant buffer `slot == 25`, e.g. `vp_c25`: `uint32_t sizes[16]` / four `uint4` holding the bound byte size of game UBOs `c[3]..c[16]`). `ubo_info` in `runtime/vk_exec_draw.c` did not handle `slot == 25` and returned `zero_info(max)`, causing every dynamically indexed UBO read (such as UI quad UV tables in `c3[14..17]`) to fail its bounds check and evaluate to `0.0`, clamping `tex 489` to pixel `(0, 0)`.
- **Fix:** Added `slot == 25` support in `ubo_info` (writing `min(ex->ubo[st][i].size, max)` for `i = 0..13` into a 64-byte scratch buffer) and `rs_build` (`rs_add` over `ex->ubo[st][0..13]`) in `runtime/vk_exec_draw.c`.
- **Verification:** Rebuilt `mtf-stage2.exe` (`1.3s`): frame `320` (`work/dump_title_c25/pass_0020_t0_2560x1440_43_rgba8srgb.png`) renders the full *Mario Tennis Fever* title artwork and logo; frame `600` (`work/dump_post_c25/pass_0038_t0_2560x1440_43_rgba8srgb.png`) renders the full translucent dialog box + `▼` prompt behind the tutorial text.

### Q21: Skinned Character Vertex Input Integer Type Mismatch (`spirv_rewrite_int_inputs`)
- **Symptom:** Skinned character shaders declare vertex attributes (e.g., bone indices) with `OpTypeFloat` in translated SPIR-V while NVN2 vertex attribute state binds `VK_FORMAT_*_UINT` or `VK_FORMAT_*_SINT` formats.
- **Root Cause:** Vulkan requires SPIR-V `OpTypeInt` vertex inputs when the pipeline vertex attribute format is integer (`UINT`/`SINT`), whereas `totk_xlate` emits `OpTypeFloat` inputs and `floatBitsToUint` / bitcasts.
- **Fix:** Added `spirv_rewrite_int_inputs` (`runtime/spirv_rewrite.h`, `runtime/spirv_rewrite.c`) and wired `vattr_int_masks` into `create_graphics` and `lib_create` (`PrKey`) in `runtime/vk_pipeline.c` to patch integer-bound vertex input variables and loads to `OpTypeInt` + `OpBitcast` back to float.
- **Verification:** Verified clean compilation and pipeline creation in `mtf-stage2.exe` with all character meshes rendering in `work/dump_800_fix/pass_0037_t0_2560x1440_43_rgba8srgb.png`.

### Q22: Missing Characters in Post-Title / Tutorial Scene (`nvnCommandBufferBeginRecording` Memory Event Saturation)
- **Symptom:** In the post-title tutorial scene (`frame 800`), rackets, ball, court, sky, UI, and Lakitu's cloud rendered, but Mario, Luigi, Umpire Toad, and Lakitu's body were completely invisible.
- **Root Cause:** When `nro0` (`f_7002fa5700` -> `f_7003057224`) bakes per-shape NVN command buffers onto a shared `NVNcommandBuffer` (`ctl_size = 256`, `game_size = 8192`), `nvnCommandBufferBeginRecording` in `runtime/nvn_api_cmd.c` only fired `OUT_OF_COMMAND_MEMORY` / `OUT_OF_CONTROL_MEMORY` callbacks when sizes were `0`, never when `hw_used` or `ctl_used` approached/exceeded `game_size` / `ctl_size`. After the first ~2–4 recordings, `o->ctl_used` (`+64` per `EndRecording`) saturated `ctl_size` (`256`) and `o->hw_used` saturated `game_size` (`8192`), so `nvnCommandBufferGetCommandMemoryUsed` and `nvnCommandBufferGetControlMemoryUsed` returned constant clamped values (`delta = 0 < 9`). `f_7003057224` treated every subsequent shape draw recording as empty and cleared its `NVNcommandHandle` (`odd_c8 = 0`), causing `f_7002fa5fe0` (`CallCommands`) to skip all remaining character shapes.
- **Fix:** Updated `nvnCommandBufferBeginRecording` in `runtime/nvn_api_cmd.c` to invoke `memory_event` for `NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY` when `!o->game_size || o->hw_used + 1024 > o->game_size` and `NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY` when `!o->ctl_size || o->ctl_used + CTL_PER_RECORDING > o->ctl_size` before recording begins.
- **Verification:** Rebuilt `mtf-stage2.exe` and dumped frame `800` (`work/dump_800_fix/pass_0037_t0_2560x1440_43_rgba8srgb.png` and `pass_0005_t0_1706x960_43_rgba8srgb.png`): Mario, Luigi, Lakitu, and Umpire Toad all render with full geometry, materials, and shadows.

### Q23: Darkened Movies and Gameplay (`_SRGB` Present Texture on `UNORM` Swapchain)
- **Symptom:** Both intro/cutscene movies and 3D gameplay appeared noticeably darker than expected (crushed midtones and shadows, as if covered by a dark overlay / `x^2.2` gamma).
- **Root Cause:** *Mario Tennis Fever* presents `NVN_FORMAT_RGBA8_SRGB` (`VK_FORMAT_R8G8B8A8_SRGB`) textures to `nvnQueuePresentTexture`, while the host swapchain in `runtime/vk_present.c` is `VK_FORMAT_B8G8R8A8_UNORM` (expecting raw sRGB bytes without hardware encode). Calling `vkCmdBlitImage` (or `vk_fsr_record` / `vk_color_record`) with `VK_FORMAT_R8G8B8A8_SRGB` caused Vulkan to decode sRGB to linear on read without re-encoding linear to sRGB on write to `UNORM`.
- **Fix:** Reinterpreted `*_SRGB` source formats as `*_UNORM` in `runtime/vk_present.c` (`present_image`) and forced `vk_color_record` (`runtime/vk.h`, `runtime/vk_color.c`) passthrough when `vfmt != fmt` so raw sRGB bytes are preserved into `R16G16B16A16_SFLOAT` / `B8G8R8A8_UNORM` without `vkCmdBlitImage` sRGB-to-linear conversion.
- **Verification:** Rebuilt `mtf-stage2.exe` and captured presented swapchain frames (`TOTK_SHOTS=780-810:15`, `work/shots_fix7/f00795.png`): brightness, shadows, sky, and character colors match reference emulator output.

### Q24: Missing Center Background in Bottom Tutorial Hint Banner (`c381fe4e8be846a3` / `67e197ca7ba86ada` Missing `else` Branch)
- **Symptom:** The bottom tutorial hint banner (`"The higher the ball, the faster the serve."`, `pass 0036` / `pass 0037`) rendered its left and right gradient end-caps (`tex 2314`, `prog 131`) with a completely transparent gap in the center (`x = [-152, +152]`).
- **Root Cause:** The center fill quad (`pass 0036`, `pipe 4`, `prog 130`, `vs c381fe4e8be846a3` / SPIR-V FNV `0x67e197ca7ba86ada`, `fs 00e4a89bd28c2bb8`) is drawn with `vp_c3[34].w == 0` (`u0[139] == 0`) and corner colors in `vp_c3[29..32]`. In the original Ampere `sm86` shader (`0x970..0xc30`), `(vp_c3[34].w & 2) == 0` branches to `0x970` to read `vp_c3[29..32]`, but `totk_xlate` dropped the `0x970..0xc30` `else` block in the translated SPIR-V, leaving the color accumulators initialized to `(clip_y, view_y, clip_x, -250.0)` and producing `alpha = clamp(-250.0 * 0.00369, 0, 1) = 0.0`.
- **Fix:** Added `spirv_ui2d_color_fix` (`runtime/spirv_rewrite.h`, `runtime/spirv_rewrite.c`) and wired it into `gfx_spirv_final` (`runtime/gfx_spirv.c`) for `0x67e197ca7ba86ada` to reconstruct the bilinear `vp_c3[29..32]` corner-color interpolation in the fallback path.
- **Verification:** Rebuilt `mtf-stage2.exe` and dumped frame `1200` (`work/dump_1200_fix11/pass_0037_t0_2560x1440_43_rgba8srgb.png`): the bottom tutorial hint banner renders as a continuous, seamless dark bar behind `"The higher the ball, the faster the serve."`.

## 20. Darker Sky and Overly Contrasted/Saturated 3D Scene Colors (`sampled_format` View Reinterpretation, Issue #13)
- **Symptom:** In the post-title 3D tutorial scene, the sky was deep navy blue instead of bright cerulean blue, clouds were grey, and the court/characters had overly dark and saturated colors (`x^{2.2}` double-gamma), while 2D UI elements had normal brightness.
- **Root Cause:** *Mario Tennis Fever*'s tonemap pass (`pass 0035`, `1706x960` `NVN_FORMAT_RGBA8_SRGB`) writes sRGB-encoded color values into an `RGBA8_SRGB` render target. The subsequent FXAA / upscale pass (`pass 0036`, `2560x1440` `NVN_FORMAT_RGBA8`) samples `pass 0035`'s texture through an `NVNtextureView` with `v->set_format = 1` and `v->format = NVN_FORMAT_RGBA8` (`UNORM`), expecting raw sRGB bytes to be filtered and copied directly into the `2560x1440` `RGBA8_UNORM` target before the 2D UI (`pass 0037`, `RGBA8_SRGB`) draws over it. Because `get_view` in `runtime/vk_exec_draw.c` always created sampled `VkImageView`s using `img->format` (`VK_FORMAT_R8G8B8A8_SRGB`) and ignored `v->set_format` / `v->format`, Vulkan hardware-decoded the sRGB values to linear (`x^{2.2}`) when `pass 0036` sampled `pass 0035`, and wrote linear values into the `UNORM` target without re-encoding to sRGB.
- **Fix:** Added `int32_t nvn_format` to `VkTexImage` (`runtime/vk.h`, `runtime/vk_texture.c`) and implemented `sampled_format` in `runtime/vk_exec_draw.c` (`get_view`) to reinterpret sampled views when `v->set_format && v->format != img->nvn_format` and both formats have identical block size (`bpb`, `bw`, `bh`) and compatible `MUTABLE_FORMAT` class.
- **Verification:** Rebuilt `mtf-stage2.exe` and dumped frame `1200` (`work/dump_1200_fix12_13/pass_0037_t0_2560x1440_43_rgba8srgb.png`): the sky is bright cerulean blue, clouds are bright white, and the grass court and characters match the reference screenshot (`media_1794851715092_91c2f428.jpg`).

## 21. Shader Lookup Stutter & `TXD` Anisotropic Gradient Transpose During Gameplay (Issue #12)
- **Symptom:** Stuttering when characters move or new animations/shaders trigger during gameplay, plus distorted anisotropic mip selection on court/terrain shaders using `TXD`.
- **Root Cause:**
  1. `nvn2_offline_get` in `runtime/gfx_shader.c` allocated a temporary buffer (`malloc` + `memcpy` + `free`) and recomputed `rt_sha256` over the full shader control + code (`up to 64 KB`) on the draw thread whenever a stage's key was looked up, and called `rt_log("gfx_shader: nvn2 loaded ...")` (synchronous disk flush) on every newly loaded shader.
  2. `Ryujinx.Graphics.Shader`'s `TXD` (`OpImageSampleExplicitLod` with `Grad`) packs `(du/dx, du/dy)` and `(dv/dx, dv/dy)` instead of SPIR-V's `(du/dx, dv/dx)` and `(du/dy, dv/dy)`.
  3. 2,520 shader stages in *Mario Tennis Fever* require pre-populated texture/attribute type overrides (`profiles/0400D4201C6EA000/shader_answers.tsv`) so vertex bone-index attributes (`uint`) and shadow/array textures match pipeline declarations without runtime mismatches.
- **Fix:**
  1. Cached the 8-byte SHA-256 key inside `prog_t`'s `owned_control` copy (`"NV2C"` tag at `+0x100` in `runtime/nvn_api_obj.c` and `nvn2_offline_get` in `runtime/gfx_shader.c`), wired `gfx_shader_load_private` to `nvn2_load_key8`, and throttled `nvn2 loaded` logs to powers of 2 (`!(n & (n - 1))`).
  2. Added `spirv_txd_transpose` (`runtime/spirv_rewrite.h`, `runtime/spirv_rewrite.c`, `runtime/gfx_spirv.c`) to transpose `OpImageSampleExplicitLod` `Grad` vectors.
  3. Imported `profiles/0400D4201C6EA000/shader_answers.tsv` (`2,520` entries) and `tools/shaders/int_inputs.py`, and updated `tools/shaders/translate.py` (`load_answers`, `sbuf_align=16`, `tess_mode=0x202`, `code[:ctl[0x38]]`, `--mvprev`).
- **Verification:** Rebuilt `mtf-stage2.exe` and verified with a 6-second run (`work/test_merge_run.log`): `nvn2 loaded` logs only on powers of 2 (`#1, #2, #4, #8, #16`) and cached NVN2 stage lookups hit in O(1) without heap allocation or SHA-256 recomputation.

## 22. `TOTK_SECONDS` vs `--seconds` CLI Flag
- **Symptom:** Running `mtf-stage2.exe` with `TOTK_SECONDS=10` in the environment kept running past 10 seconds at 60 FPS.
- **Root Cause:** `runtime/main.c` parses `--seconds <N>` on the command line rather than `TOTK_SECONDS` / `MTF_SECONDS`.
- **Fix / Usage:** Pass `--seconds <N>` directly on the CLI (`.\build\v327680_stage2\mtf-stage2.exe --seconds 8`), which exits cleanly with status `0` after `N` seconds.

## 23. Issue #14 — Washed-Out / Whitish 3D Scene in SDR
- **Symptom:** On the practice court in SDR (`TOTK_HDR=0`), the 3D scene (grass, Mario, tents, sky) appears washed out/whitish, while the 2D UI and the background stadium on the right have normal contrast.
- **Root Cause:** The pass generating the `8x8x64` aerial perspective 3D LUT (`0x804bff0000`, `draws 64`, `layers 0+64`, `gs=NULL`) uses `vs f444e5b3aba250a3` without `BuiltIn Layer`. All 64 draws overwrite only `slice 0` (leaving `slice 0` with the distant sky value `[52, 145, 255]` and `slices 1..63` at `[0, 0, 0]`). `pass 0013` (`fs 2795532640a31474`) then applies `slice 0`'s fog over all geometry near the camera, washing out the court and characters.
- **Fix:** Skip draws of the `8x8x64` pass in `runtime/vk_exec_draw.c` (`prepare_draw_impl`), keeping the volume at `(0, 0, 0)` without spurious foreground fog.
- **Verification:** Automated gameplay capture (`work/practice.script`, `TOTK_DUMP=650`) validated with colors, shadows, and contrast matching the Switch 2 reference.

## 24. Issue #15 — Excessive Clouds / Sky Coverage vs. Switch 2 Reference
- **Symptom:** After fixing colors in Issue #14, the sky on the practice court shows excessive white cloud coverage over the blue background, whereas the Switch 2 reference is predominantly blue with fewer clouds visible.
- **Next step:** Inspect the sky/cloud pass (`pass 0009` / `pass 0012` `341x480 rgba16f`) and verify whether the difference comes from `TCS`/`TES` (cloud sea tessellation) or from the cloud shader/texture.

## 25. Issue #16 — Black Box Stuttering Around Characters During Gameplay
- **Symptom:** Intermittent black boxes around characters during gameplay (reproducible in the tutorial after serving and walking around the court), plus Mario's shadow stuttering in the tutorial.
- **Diagnostics:** Added `F7` hotkey (`runtime/gfx_window_win.c`, `runtime/vk_dump.c`) to toggle continuous log capture (`[host]` + pass/frame summaries in `logs/capture_<n>.log`) without PNG readback.
- **Next step:** Capture logs with `F7` during shadow/black-box stuttering and test `X` (Toss) + movement in `INPUT_SCRIPT`.

## 26. `MTF_FPS_LIMIT > 60` Accelerates Game Simulation (1:1 Fork)
- **Symptom:** With `VK_PRESENT=immediate` and `MTF_FPS_LIMIT=144` in a real match, characters move faster (~2.4x simulation speed) instead of keeping `1x` speed with more frames.
- **Root Cause:** Overriding only the global target FPS pointer (`0x70035611b8` read by `f_7003061ab8` in `nro0`) uncaps the `nn::os::SleepThread` pacer to 144/1900+ FPS, while game logic/physics still uses a fixed `1/60s` step per frame.
- **Fix (Implemented via Frame Gen):** Keep simulation at `60 Hz` and achieve `120–240 FPS` at `1x` real speed via optical-flow Frame Generation (`MTF_FRAMEGEN` in `runtime/vk_framegen.c`). Native delta-time decoupling remains optional/future.

## 27. Lightning Racket `VK_ERROR_DEVICE_LOST` (`0fa55a01f31ef9a6` / `SSign` on Blackwell `610.88`)
- **Symptom:** Starting a match with the Lightning Racket froze and crashed with `VK_ERROR_DEVICE_LOST (-4)` (`instruction pointer (invalid) at 0x200xxxxxx`) on RTX 5070 Ti (driver `610.88`).
- **Root Cause:** Binary search inside `0fa55a01f31ef9a6.spv` (reproduced locally in standalone Vulkan) isolated `GLSL.std.450` `SSign(int32)` (`ExtInst 5`), whose `-1/0/+1` result is bitcast to `float` and used downstream, triggering an NVIDIA Blackwell `610.88` SPIR-V -> SASS compiler bug.
- **Fix:** `spirv_fixup` in `runtime/vk_pipeline.c` rewrites `GLSL.std.450` `SSign(int32)` to `SClamp(x, -1, 1)` (`ExtInst 45`).
- **Verification:** Standalone Vulkan repro switched from `vkQueueWaitIdle: -4` to `0`, and Lightning Racket match ran cleanly.

## 28. `nvoglv64.dll+0x10ea90` Access Violation in Character Select / Free Play (`spirv_rewrite_inputs`)
- **Symptom:** Navigating characters in Free Play or after several matches crashed in `create_graphics` / `lib_create` (`vkCreateGraphicsPipelines`, `runtime/vk_pipeline.c`) at `nvoglv64.dll+0x10ea90` reading `0x8`.
- **Root Cause:** When `k->attr_type != 0` (e.g., `attr=0xc0`), `spirv_rewrite_inputs` in `runtime/vk_pipeline.c` emitted newly generated `OpTypeInt` / `OpTypeVector` / `OpTypePointer` instructions at `fn_at` (`OpFunction`) — **after** global `OpVariable` declarations that referenced them, producing an illegal SPIR-V forward type reference that crashed `nvoglv64.dll`'s SPIR-V parser.
- **Fix:** Updated `spirv_rewrite_inputs` (`6abc64f`) to insert newly created `OpType*` definitions at `var_at` (before the first `OpVariable`), and kept synchronous pipeline builds on NVIDIA (`5076a61`) with `VkPipelineCache` enabled (`153d2f6`).
- **Verification:** Passing through all characters in Free Play and boot prewarm (`1,389` pipelines, `0` failed) run cleanly without crashing.

## 29. Character Select Crash with `MTF_REPLAY_THREADS=6` (`static` Scratch Arrays in `vk_exec_draw.c`)
- **Symptom:** Entering the character selection screen (first pass with `>= 256` draws), right after `vk: 5 replay slice workers started` in `mtf.log`, the game exited immediately.
- **Root Cause:** In `runtime/vk_exec_draw.c`, temporary `static` arrays (`rs_list`, `bind_resources`), `wait_frame`/`wait_left_ns`, and `vk_dlaa_draw` / `mvobj_draw` calls were shared globally across the 6 slice worker threads, causing concurrent descriptor corruption.
- **Fix:** Moved `rs_tmp`, `bind_w`, `bind_d`, `bind_bv`, `wait_frame`, and `wait_left_ns` into per-thread `DrawCtx`; moved `vk_dlaa_draw` to collection order (`par_take`) and gated `vk_dlaa_draw`/`mvobj_draw` inside `prepare_draw`/`op_draw_idx` on `!ex->draw`.
- **Verification:** Clean rebuild (`0` errors), boot smoke test (`alive=True`), `python tools/check_content.py` (`0` violations), and audited `DC` (`_Thread_local`), `cache_lock`, `oq_outside`, `repeat_stats`, and `count_quiet`.


## 30. AMD RDNA 2 Match-Entry Crash, Light-Blue Wash & Remaining Ball-Trail Quirk
- **Symptom:** On AMD (`amdvlk64.dll 24.30.66`), entering a match crashed (`GeometryShaderPassthroughNV` / `ViewportMaskNV` unsupported). Skipping all `5251` geometry shaders prevented the crash but left the `8x8x64` atmosphere LUT unpopulated (light-blue wash over 3D scenes).
- **Fix:** `runtime/vk_pipeline.c` (`lower_gs_passthrough`) lowers `GeometryShaderPassthroughNV` (`5251`) `gl_Layer` shaders to standard Vulkan Geometry Shaders (forwarding `Location 0`), and `spirv_fixup` strips excess `CullDistance` from both `gl_PerVertex` blocks. Only `ViewportMaskNV` (`5253`/`5255`) is skipped when `!has_viewport_array2`.
- **Remaining Quirk:** On AMD RDNA 2 (`24.30.66`), A/B/Y shot ball trails render only their outer edges despite identical shaders/textures (`pass 0017`/`0019` vs NVIDIA `pass 0081`) and passing `MTF_PIPE_SKIP=0`, `MTF_MEMORY=mirror`, `MTF_FEEDBACK=restart`, `MTF_ASTC_BC=0`, `MTF_NO_EDS3=1`, and `MTF_NAN_GUARD=mark`.

## 31. HDR10 Over-Exposure & Unresponsive `Game Brightness` Slider
- **Symptom:** With `HDR=1`, sunlit court grass clipped brightly and dragging `Game Brightness` (`HDR_PAPER_WHITE`) / `Display Peak Nits` (`HDR_PEAK_NITS`) produced no visible change when the window was smaller than `1080p` or used `FSR`.
- **Root Cause:** `hdr_compose.comp` previously gated scene HDR on `abs(srgb_encode(sdr) - w) <= 1.5/255` (falling back to `UI Brightness` whenever window scaling/upscaling altered `w`) and applied `lift_shadows()` (`pow(l, 0.82)`) + an early `knee(0.90f)`.
- **Fix:** Removed `lift_shadows()` and the fragile `1.5/255` match gate in `runtime/shaders/hdr_compose.comp` (`hdr_compose_spv.h`), raised the knee to `0.97f` with a `2.0x` headroom cap in `runtime/vk_hdr.c`, and locked `Render Scale = 100%` + `Upscaler = Bilinear` in `runtime/ui/menu.cpp` while `HDR` is active.

## 32. Firefox WebCodecs Green Border & AV1 Stream Crop
- **Symptom:** Remote players joining `F1 -> Multiplayer` on Firefox saw a large green padding border (video shrunk to top-left corner) when the stream negotiated AV1.
- **Root Cause:** `stream_hwenc.cpp` did not set `MF_MT_DEFAULT_STRIDE` explicitly on NV12 input, and `stream_host.c` drew decoded `VideoFrame`s via `ctx.drawImage(f, 0, 0, c.width, c.height)` without cropping to `f.visibleRect`, while Firefox's WebCodecs AV1 decoder exposed padded coded dimensions.
- **Fix:** Set `MF_MT_DEFAULT_STRIDE = w` in `runtime/stream_hwenc.cpp`, updated `drawImage` in `runtime/stream_host.c` to use `f.visibleRect`, and skipped `av1` on Firefox user-agents (`av1=false`) so Firefox uses **H.264** (with VP8 fallback) while **AV1** (`AV1 -> H.264 -> VP8`) remains enabled on Chromium-based browsers.
