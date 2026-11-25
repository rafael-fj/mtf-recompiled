# MTF Recomp — Frequently Asked Questions (FAQ)

Common questions and troubleshooting for **MTF Recomp** (*Mario Tennis Fever* PC port).  
If your issue is not listed here, check `logs/mtf.log` (or press **F1 → Advanced → Open log**) and open an issue on [GitHub Issues](https://github.com/rafael-fj/mtf-recompiled/issues).

---

## Multiplayer & Controllers

### Why is Local Multiplayer not working / Player 2 not detected?
By default, multi-controller routing may be disabled or both players may not have distinct devices assigned.
1. Press **F1** (or **Esc**) to open the overlay and go to **Controls**.
2. Enable **Multiplayer input** (`MULTIPLAYER_INPUT = 1` in `mtf.ini`).
3. Assign a separate input device for each player (`P1`–`P4`) — for example, `P1 = pad:0` (first controller) and `P2 = pad:1` (second controller) or `keyboard`.
4. In the game's **Free Play** mode, make sure you select a 2-player (or 4-player) setup on the character/court screen.

### How do I play Online with friends?
MTF Recomp includes built-in **Browser Remote Play** so friends can join from any modern browser (Chrome, Edge, Firefox) without installing the game:
1. Open **F1 → Multiplayer**.
2. Choose a hosting mode:
   - **Cloudflare Tunnel** (easiest for playing over the internet without port forwarding).
   - **Local Network / LAN** (best latency if you are on the same network or VPN/ZeroTier/Tailscale).
3. Start hosting and share the generated URL/room link with your friends.
4. Friends open the link, connect a controller (or use keyboard), and automatically take `P2`–`P4`.
5. **Tips for best stream quality:**
   - Chromium-based browsers (Chrome/Edge) use **AV1**, while Firefox automatically uses **H.264/VP8**.
   - Adjust **Stream Quality** (`Performance`, `Balanced`, `Quality`) in **F1 → Multiplayer** if someone experiences stuttering or high latency.
   - If UDP/WebRTC is blocked by a strict firewall/NAT, the host automatically falls back to **WebSocket**.

### Why are the in-game button icons wrong (e.g. Xbox icons on a Switch/PlayStation pad)?
MTF Recomp automatically switches button prompts live based on the last input device used (Keyboard, Switch, Xbox, or PlayStation). Press a button on your desired controller in-game, or check **F1 → Controls** if your pad is being mapped as a generic XInput device.

---

## Performance, FPS & Stuttering

### Why does the game run in slow motion below 60 FPS, and how do I fix it?
*Mario Tennis Fever* runs on a fixed 60 Hz simulation loop: if your PC renders at 30–45 FPS, the game normally slows down proportionally.
- Open **F1 → Screen** and enable **Dynamic FPS** (`DYNAMIC_FPS = 1`).
- **Dynamic FPS** keeps the game's simulation running at **60 ticks/sec (1x real-time speed)** even when your GPU renders below 60 FPS, eliminating slow motion.

### Can I play at 120+ FPS without speeding up the game?
- **Yes, using Frame Generation:** Go to **F1 → Screen → Frame Generation** and set a target (`120 FPS`, `Display`, or `Unlimited`). This synthesizes intermediate frames via optical flow while keeping the 60 Hz simulation intact.
- **Note on `FPS Limit`:** Setting `FPS Limit` above `60` *without* Frame Generation will speed up gameplay, because the engine's animations, timers, and physics are tied to 60 Hz ticks. Keep `FPS Limit` at `60` when using Frame Generation.

### Why does the first launch or first match stutter briefly?
On the first run (or after a GPU driver update), Vulkan compiles shader pipelines in the background (`pipeline-cache.bin`). MTF Recomp uses a non-blocking 3-second boot prewarm and async pipeline workers on NVIDIA GPUs to keep startup fast, so brief one-time stutters can occur the first time a new effect or court appears. Subsequent launches reuse the cache and are smooth.

### Is the game playable on an integrated GPU (iGPU)?
Yes! For low-spec PCs or handhelds/iGPUs:
1. Set **F1 → Graphics → Graphics Preset** to **Low** (or **Recommended**).
2. Enable **F1 → Screen → Dynamic FPS**.
3. Verified on an **AMD Radeon(TM) Graphics** iGPU (`0x1002:0x13c0`, `low` preset): averages **42.6–45.1 FPS** in-match while maintaining **60 ticks/sec** (full 1x gameplay speed).

---

## Graphics, DLSS & HDR

### Why is NVIDIA DLSS / DLAA disabled (grayed out) or doing nothing?
DLSS and DLAA require **both**:
1. An **NVIDIA RTX** graphics card (GTX / AMD / Intel GPUs cannot run DLSS; use **FSR 1** or **SMAA/FXAA** instead).
2. The `nvngx_dlss.dll` library placed next to `mtf-recomp.exe`:
   - In **MTF-Recomp-Setup.exe**, keep the **Download NVIDIA DLSS** option checked (it automatically checks if `nvngx_dlss.dll` is already installed and skips on non-RTX GPUs).
   - In **RecompLauncher**, make sure the optional `NVIDIA DLSS` (`nvidia.dlss`) component is enabled.

### How do I enable and calibrate HDR?
1. Enable **HDR** in **Windows Settings → System → Display** first.
2. Open **F1 → Graphics**, turn on **HDR**, and click **Restart** when prompted.
3. Once restarted, adjust **Peak Nits**, **Paper White**, and **UI Nits** live in **F1 → Graphics** to match your display.
4. *Note:* While HDR is active, `Render Scale` is locked to `100%` and `Upscaler` to `Bilinear` (and SDR color sliders are disabled) to preserve the 10-bit HDR signal.

### Why did my Graphics Preset change to "Custom"?
Changing any individual quality setting (**Render Scale**, **Upscaler**, **Anisotropic Filtering**, **Shadow Scale**, **Tessellation**, **FXAA**, or **SMAA**) automatically switches `Graphics Preset` to `Custom` so your manual tweaks aren't overwritten.

---

## Installation, Updates & Saves

### What game files do I need?
You need your own legally dumped copy of *Mario Tennis Fever* updated to **Version 1.1.1 (`v327680`)**:
- Either a combined `.nup` file (base game + Update 1.1.1), **or**
- An extracted Update `1.1.1` folder containing `exefs/` and `romfs/`.  
No `prod.keys` or Switch firmware files are required.

### Will updating MTF Recomp erase my save data or settings?
No.
- **RecompLauncher:** updates are applied automatically while preserving your `profile/` save folder and `mtf.ini`.
- **Standalone (`MTF-Recomp-Setup.exe`):** extract the new release zip and run `MTF-Recomp-Setup.exe` pointing to the same install folder — your saves and `mtf.ini` remain untouched.

### Where are my settings, saves, and logs stored?
All user files live in your install folder next to `mtf-recomp.exe`:
- **Settings:** `mtf.ini`
- **Save data:** `profile/`
- **Logs:** `logs/mtf.log` (also accessible via **F1 → Advanced → Open log**)
