# MTF Recomp

<p align="center">
  <a href="https://recompstore.com/dev/rafael-fj"><img src="https://recompstore.com/assets/press/verified-dark.svg" alt="Verified developer on RecompStore"></a>
  <a href="https://recomplauncher.com/game/mtf-recompiled"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>
</p>

<p align="center">
  <a href="https://github.com/rafael-fj/mtf-recompiled/releases/latest"><b>Download</b></a> ·
  <a href="#get-started">Get started</a> ·
  <a href="docs/port-status.md">Status</a> ·
  <a href="docs/faq.md"><b>FAQ</b></a> ·
  <a href="#multiplayer--browser-remote-play-f1---multiplayer">Multiplayer</a> ·
  <a href="https://github.com/rafael-fj/mtf-recompiled/issues">Issues</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="#respect-copyright">Respect copyright</a>
</p>

**MTF Recomp** is a native Windows `x86-64` static recompilation of *Mario Tennis Fever* (Nintendo Switch 2, Update `1.1.1` / `v327680`, Title ID `0400D4201C6EA000`) with a Vulkan 1.3 renderer, built locally from your own copy of the game.

<p align="center">
  <img src="docs/images/main-menu.jpg" alt="Main Menu in MTF Recomp" width="24%">
  <img src="docs/images/fever-rackets.jpg" alt="Fever Rackets in MTF Recomp" width="24%">
  <img src="docs/images/jungle-court.jpg" alt="Jungle Court in MTF Recomp" width="24%">
  <img src="docs/images/waluigi-pinball.jpg" alt="Waluigi Pinball in MTF Recomp" width="24%">
</p>
<p align="center"><sub>Screenshots taken in MTF Recomp (144 FPS). The game is © Nintendo / Camelot; you need your own copy to run it.</sub></p>

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Mario Tennis Fever*, updated to **version 1.1.1 (`v327680`)** and dumped from **your own** Nintendo Switch 2. This project does not provide game files, keys, firmware, dumping or decryption tools, or instructions on how to obtain them. The game code is recompiled locally, on your machine, from your files.

> [!NOTE]
> **Built with AI assistance.** Parts of the port enhancements, tooling, UI, and documentation were written with AI coding assistants, directed, tested, and reviewed by a human. It builds upon upstream open-source MIT recompilations ([MTF Recomp `0.1.0+6caba99`](https://github.com/kayochiaradia/mtf-recomp), [TotK Recomp](https://github.com/valentderah/totk-pc-recomp), [LAS Recomp](https://github.com/peiikai/LASRecomp), [MacNativeGames](https://github.com/ly05010419/MacNativeGames), [nxnative](https://github.com/seedds/nxnative), and [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)).

---

## Features

- **Full game from your own copy:** Adventure, Tournament, Trial Towers, Free Play, Mix It Up, and local multiplayer.
- **High Frame Rates, Dynamic FPS & Frame Generation:** `60 FPS` native simulation, **Dynamic FPS** (`F1 -> Screen`) to keep `60 Hz` real-time speed when rendering drops below 60 FPS (ideal for iGPUs/low-spec PCs), unlocked engine `FPS_LIMIT` (`30`–`Uncapped`), and optical-flow Frame Generation (`120`, monitor refresh e.g. `144 Hz`, or `unlimited`).
- **High Resolution & Upscaling:** `720p` to `8K (4320p)`, smart `auto` matching your display, `50%`–`200%` render scale, AMD FSR 1, Bilinear, HDR10, and NVIDIA DLAA/DLSS.
- **Controllers & Dynamic Prompts:** Plug-and-play SDL3 gamepad support (Nintendo Switch, Xbox, PlayStation), full keyboard/gamepad remapping (`F1 -> Controls`), and live-switching button prompt icons matching your active input device.
- **In-Game Overlay (`F1` / `Esc` / `Minus+Plus`):** Dear ImGui settings menu in English and Brazilian Portuguese (`pt-BR`), performance monitor, and `F1 -> Cheats` (in-memory toggles to unlock characters, outfits, rackets, courts, and cups/modes for instant multiplayer without altering your save).
- **Multiplayer & Browser Remote Play (`F1 -> Multiplayer`):** Local multi-gamepad routing (`P1`–`P4`) plus built-in browser streaming (`AV1` on Chromium / `H.264` on Firefox + `Opus` + remote controller input) so up to 4 friends can join from a browser.
- **Easy Setup & Updates:** RecompLauncher SDK integration (`recomplauncher.json`, Levels 1–3), guided Windows installer (`MTF-Recomp-Setup.exe`), and an automatic in-game GitHub release notifier (one-time 5s banner + `F1` menu reminder).

---

## Get started

**You need:** your own legal game files (`Update 1.1.1 / v327680`, `.nup` or extracted `exefs/` and `romfs/`), Windows 10 or 11 (x86-64), a GPU with Vulkan 1.3, 16 GB of RAM, and about 10 GB of free disk space.

### Option A — RecompLauncher (Recommended)
1. Install [RecompLauncher](https://recomplauncher.com).
2. Install **Mario Tennis Fever: Recompiled** and pick your `.nup` (base game + Update 1.1.1) or your extracted `Update 1.1.1` folder (`exefs/` and `romfs/`); the launcher builds the game for you.
3. Adjust graphics, audio, and cheats directly in the launcher and press **Play**.

### Option B — Windows Setup (`MTF-Recomp-v*-Windows-x64.zip`)
1. Download `MTF-Recomp-v*-Windows-x64.zip` from the [latest release](https://github.com/rafael-fj/mtf-recompiled/releases/latest) and extract it to a writable folder (not `Program Files`).
2. Run `MTF-Recomp-Setup.exe` (English / `pt-BR`), select your `.nup` or extracted `1.1.1` folder, and pick your install destination.
3. Launch `mtf-recomp.exe` (`F1` or `Esc` opens the settings overlay; `Alt+F4` closes the game cleanly).

### Option C — Build from Source (`tools/build_fork.py`)
1. Enable the pre-commit content guard:
   ```powershell
   git config core.hooksPath .githooks
   ```
2. Copy `config.example.toml` to `config.local.toml` and set `[game].dump_path` to your extracted *Mario Tennis Fever + Update 1.1.1* folder.
3. Build with CMake + Ninja (`build/fork/bin/mtf-recomp.exe`):
   ```powershell
   python tools/build_fork.py
   ```
4. Verify zero game content is tracked:
   ```powershell
   python tools/check_content.py
   ```

**Updating:** In RecompLauncher, updates are automatic and keep your saves (`profile/`) and settings (`mtf.ini`). For standalone installs, extract the new release zip and run `MTF-Recomp-Setup.exe` pointing to the same install folder — your saves and `mtf.ini` remain untouched.

---

## Saves

Saves are stored in the `profile/` folder next to `mtf-recomp.exe` (alongside `mtf.ini` for settings). Close the game and copy `profile/` to back up or move your saves.

---

## Multiplayer & Browser Remote Play (`F1 -> Multiplayer`)

| Option | Transport | When to Use | Public IP Privacy | Expected Delay |
| :--- | :--- | :--- | :--- | :--- |
| **Local Host** | HTTP + WebSocket (direct LAN / port-forward) | Same local network (LAN/Wi-Fi) or trusted peers with zero tunnel overhead | Exposed if shared over WAN/port-forward (privacy warning shown) | **~15–35 ms** (LAN) |
| **Cloudflare (WebSocket)** | HTTP + WebSocket via `trycloudflare.com` tunnel | Playing over the internet when strict NAT/firewalls block UDP or you want a zero-config link | **Hidden** behind `trycloudflare.com` | **~45–90 ms** (depends on region/route) |
| **WebRTC (Low Latency UDP)** | WebRTC DataChannel (UDP) + automatic WebSocket fallback | Best responsiveness for online matches; falls back to WebSocket automatically if a peer's NAT blocks UDP | **Hidden** during signaling when `cloudflared.exe` is installed | **~25–55 ms** (direct UDP) |

- **Stream Quality Presets:** Switch live in `F1 -> Multiplayer` between **Performance** (`540p`, `2.2 Mbps`), **Balanced** (`720p`, `4.5 Mbps`, default), and **Quality** (`720p`, `7.5 Mbps`).
- **Browser Compatibility & Codecs:** **AV1** streaming (`AV1 -> H.264 -> VP8`) requires a **Chromium-based browser** (Chrome, Edge, Brave, Opera). **Firefox** is supported using **H.264** (with VP8 fallback) only — AV1 is automatically skipped on Firefox.
- **Notifications:** A 3-second banner at the top of the screen notifies the host when remote players (P2–P4) connect or disconnect.
- **One-Click Tunnel Setup:** Install `cloudflared.exe` directly from `F1 -> Multiplayer` (**Install Cloudflare**).

---

## Status

| Area | Status | Progress | Details |
| :--- | :--- | :--- | :--- |
| **ARM64 Static Lifting** | Complete | ![99%](docs/images/bar-99.svg) | 110,464 functions lifted (`main`, `sdk`, `nro0`) |
| **Horizon OS HLE & Kernel** | Playable | ![95%](docs/images/bar-95.svg) | Clean boot, saves, preemption, in-memory Cheats |
| **Vulkan Renderer (NVIDIA)** | Playable | ![96%](docs/images/bar-96.svg) | 0 validation errors (Gate F5), HDR10, DLAA/DLSS, Frame Gen |
| **Vulkan Renderer (AMD)** | Playable (Partial) | ![82%](docs/images/bar-82.svg) | Boots & plays with fixed 3D colors; A/B/Y ball-trail interior missing on RDNA 2 (`24.30.66`) |
| **Audio & Video** | Complete | ![98%](docs/images/bar-98.svg) | `audren` REV15 (96 voices) + `libvpx` VP9 cutscenes |
| **UI, Controls & HDR** | Playable | ![95%](docs/images/bar-95.svg) | `F1` overlay, draggable sliders (`Ctrl+Click` type), HDR10 locks |
| **Multiplayer & Remote Play** | Playable | ![92%](docs/images/bar-92.svg) | Local `P1`–`P4` + WebRTC / Cloudflare / Local Host streaming |
| **Installer & Launcher** | Ready | ![98%](docs/images/bar-98.svg) | Self-contained Windows x64 zip + RecompLauncher SDK |

See **[docs/status-details.md](docs/status-details.md)** for what each percentage means and verified vs. known gaps, **[docs/port-status.md](docs/port-status.md)** for module metrics, and **[docs/roadmap.md](docs/roadmap.md)** (`F0`–`F9`) for the milestone checklist.

---

## Controls

Any controller SDL3 knows works out of the box. Default keyboard bindings and shortcuts:

| Action | Key | Action | Key |
| :--- | :--- | :--- | :--- |
| **Move / Aim (Left Stick)** | `W` `A` `S` `D` | **Right Stick** | Arrow Keys |
| **A / B** | `K` / `J` | **X / Y** | `I` / `U` |
| **L / R** | `Q` / `E` | **ZL / ZR** | `Z` / `C` |
| **Plus (+) / Minus (-)** | `Enter` / `Backspace` | **Motion / Shake** | `Space` |
| **Settings Overlay** | `F1` / `Esc` / `Minus+Plus` | **Fullscreen** | `F11` |
| **Quit Game** | `Alt + F4` (or `F1 -> Quit`) | | |

Every gameplay action can be rebound in **`F1 -> Controls`**.

---

## Found a bug?

1. Check [docs/obstacles-and-quirks.md](docs/obstacles-and-quirks.md) and search [existing issues](https://github.com/rafael-fj/mtf-recompiled/issues) first.
2. Make sure you are on the latest release and game version `1.1.1 (v327680)`.
3. [Open an issue](https://github.com/rafael-fj/mtf-recompiled/issues/new) attaching your **`logs/mtf.log`** (next to `mtf-recomp.exe`), your GPU model/driver version, and steps to reproduce. **Never** attach game files, `.nup` archives, or keys.

---

## Contributing and building

Pull requests are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) first and make sure `python tools/check_content.py` passes before opening a PR.

Additional technical documentation:
- [docs/devlog.md](docs/devlog.md) — Development and reverse-engineering log
- [docs/obstacles-and-quirks.md](docs/obstacles-and-quirks.md) — Known MTF-specific technical obstacles and solutions
- [docs/switch2-recomp-rag.md](docs/switch2-recomp-rag.md) — Cross-project Switch 2 recompilation RAG & license guide

---

## Respect copyright

- *Mario Tennis Fever* is © Nintendo / Camelot Software Planning. This is a fan project. It is not affiliated with, endorsed by, or sponsored by Nintendo or Camelot. *Nintendo Switch* and *Mario Tennis* are trademarks of Nintendo.
- This repository contains no game files, no game code (including no lifted C code generated from the game), no assets, no fonts, and no keys — and release builds never will either.
- The game code you generate locally and the executable compiled from it contain Nintendo's code. They are for your own personal use only: **do not share, upload, or distribute them**.
- Issues, pull requests, or discussions containing game files, keys, generated game code, or links to pirated copies will be removed immediately. Piracy is not supported in any way.
- Follow the copyright laws of your country. Dumping a game you own for personal use is legal in some jurisdictions and not in others; that responsibility rests with you.

---

## Credits and license

Project-owned code is licensed under the [MIT License](LICENSE). The license covers this project's own code only and grants nothing for Nintendo's game, which you must supply yourself.

Originated from the `0.1.0+6caba99` (MIT) release of [kayochiaradia/mtf-recomp](https://github.com/kayochiaradia/mtf-recomp). All third-party components, commits, and licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
