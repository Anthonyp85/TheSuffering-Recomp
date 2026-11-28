# The Suffering — Unofficial Original Xbox to Native PC (Windows & Linux / Steam Deck) Static Recompilation

> **Unofficial Clean-Room Project Disclaimer:** This is an **unofficial, independent, community-developed** static recompilation of the Original Xbox version of *The Suffering (USA)* (`Title ID 0x4D570015`, XDK `1.0.5788.1`) built on the [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) SDK. It is **not affiliated with, authorized, maintained, sponsored, or endorsed by** Surreal Software, Midway Games, Warner Bros. Discovery / Warner Bros. Entertainment, Microsoft Corporation, Valve Corporation, NVIDIA Corporation, Advanced Micro Devices, Inc., or Intel Corporation. *The Suffering*, Xbox, Direct3D, Steam Deck, NVIDIA RTX, GeForce RTX, DLSS, AMD FidelityFX, and Intel XeSS are trademarks and/or registered trademarks of their respective owners.

---

## Legal & Asset Policy

- This repository and its release packages contain **zero** copyrighted game assets, Xbox disc images (`.iso` / `.xiso.iso`), Xbox executables (`.xbe`), or game-derived recompiled code (`suffering.dll` / `libsuffering.so` / `generated/`).
- **You must supply your own legally acquired copy** of the Original Xbox disc image for **The Suffering (USA)**.
- Your disc image is always opened **strictly read-only** and mounted directly in place as `D:\` without extracting or copying the 2.38 GB image.
- Original wrapper, pre-game graphical launcher, Dear ImGui settings overlay, in-app XISO file browser, upscaling suite, hooks, icon generator, resource script, Steam Deck installer, verification harnesses, and build scripts in this repository are licensed under **`GPL-2.0-or-later`** with the **Runtime Upscaler Plugin Exception** (see [LICENSE](LICENSE)). **Zero** `.dll` or ELF binaries are stored in this Git repository or `source.zip`; end-user Windows `win64-launcher.zip` releases bundle the 4 unmodified official vendor-signed upscaler DLLs and their vendor license notices in `plugins\` under that exception, while end-user Native Linux `linux-x86_64.tar.gz` releases ship 100% open-source native Vulkan 1.2 + SDL3 binaries and self-contained LLVM compiler tools. All third-party licenses and notices (XboxXBERecomp MIT, Zydis/Zycore MIT, toml++ MIT, Dear ImGui MIT, SDL3 zlib, glslang/SPIRV-Tools BSD-3-Clause/Apache-2.0, zlib, LLVM Apache-2.0 WITH LLVM-exception, AMD FSR 1.0 / CAS / `ffx_api` MIT + AMD Binary Redistribution License, NVIDIA NIS MIT + NVIDIA RTX SDKs License, Intel Simplified Software License, and MSVC CRT) are preserved in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## What's New in `v1.6.2`

- **Renderer Synchronization & Window Resize Target Lifecycle Fixes (D3D12 & Vulkan):**
  - Added external `VkSubpassDependency` attachment synchronization on Vulkan presentation/UI passes and color/depth render-pass reentry (omitting empty dependencies on attachment-free passes) so prior attachment writes remain visible across same-layout passes.
  - Fixed D3D12 and Vulkan window resizes leaving stale render targets or invalidating a live Vulkan front target on acquire retries: scene render extents are now held until post-present `ApplySettings()`, which detects the extent change, waits for GPU completion, invalidates targets, updates coordinate scales, and resets upscaler history.
- **High-FPS (`>60 Hz`) Cloth Pacing & Options Slider Persistence Fixes:**
  - Fixed cloth simulation (`sub_001A6480`) running too fast above 60 FPS by subtracting the exact `1/60s` step passed to the guest and sharing one scheduling decision per guest frame across all cloth objects.
  - Fixed dragging an in-game Options slider away and back to its saved value persisting an intermediate value: the current settings snapshot now always mirrors the guest while equality with the saved baseline gates disk writes only.
- **Controller-Only Input Recording Contract (`#TSREC 1`), Settings Persistence & Key-Rebind Policy:**
  - Made `--record-input` / `XBE_INPUT_RECORD` explicitly controller-only with a non-modal in-game recording HUD notice, disabling native mouse/keyboard, menu mouse, guest preference overrides, cheats, and native stair assists identically to replay/autotest without altering saved controls or breaking legacy `#TSREC 1` recordings (recording native mouse/keyboard is not supported by the `#TSREC 1` format).
  - Cleared matching temporary CLI override guards when explicitly editing display mode, resolution, skip-launcher, or clicking **Reset Defaults** in the Pre-Game Launcher or `F1` overlay, while keeping unrelated saves non-destructive to temporary CLI flags.
  - Enforced a shared host-reserved key policy (`MkbCanBindKey`) across Win32 and SDL so reserved host keys (`F1`, `F2`, `F3`, `F10`, `LWin`/`RWin`) cannot be bound into non-functional bindings.
- **Windows Self-Updater Non-ASCII & Metacharacter Path Safety:**
  - Replaced the Windows `update_apply.cmd` helper with an ASCII PowerShell script (`update_apply.ps1`) that decodes UTF-8 paths and uses literal filesystem operations and `.NET` process relaunch without `cmd.exe` or wildcard expansion, preserving `settings.toml`, Xbox saves (`hdd\`), media, and caches on Unicode or special-character install paths.

### Previous Highlights (`v1.6.1`)
- **Cheat Menu `PopStyleColor()` Crash Fix & Full ImGui Stack / Gamepad / Modal Hardening:**
  - Fixed a Dear ImGui `g.ColorStack.Size` underflow crash when toggling cheats inside the **`F2` / `F1 -> Cheats`** overlay, and deployed RAII `ScopedStyleColors` / `ScopedStyleVars` / `ScopedDisabled` guards, `BeginTooltip()` guards, DPI font-atlas invalidation, and modal/rebind gamepad hardening across all UI modules.
- **`10 Docks` Wooden / Railed / Long-Flight Staircase Fix (`sub_00179D60` & `sub_001709C0`):**
  - Fixed Torque clipping through, wedging against side handrails on, or getting blocked on wooden, switchback, and multi-flight staircases in **`10 Docks`** and **`4a/4b Quarry`**.
- **`OneHitKills` `8b Bluff` Hermes Boss Softlock Guard & `hooks.cpp` FPU / Memory Bounds Hardening:**
  - Excluded environmental-puzzle bosses (`CHermes`, `CHorace`, `CKilljoy`) and sentinel-HP setpiece actors (`>= 5,000 HP`) from **One-Hit Kills**, and added `SavedX87State` FPU preservation and `ValidGuest` bounds checks.
- **Windows Release `-O3 -DNDEBUG` Optimization, Zero-Path-Leak Deterministic Builds & Binary Hygiene Gate:**
  - Enforced explicit `-O3 -DNDEBUG` Release flags across Windows & Linux Clang presets, deterministic `/Brepro` + `/PDBALTPATH:%_PDB%` / `--build-id=sha1 --strip-debug` linking, `-ffile-prefix-map` path remapping, and automated PE/ELF/COFF binary path-leak scanning (`gate2b_binary_hygiene`).

### Previous Highlights (`v1.6.0`)
- **In-Game Cheat Menu (`F2` Quick-Toggle or `F1` / `BACK + L3` -> `Cheats`) & Native Main Menu `"Quit Game"`:**
  - Added a native **"Quit Game"** button directly below **"Bonus Material"** on the in-game Main Menu (matching Original Xbox typography, horror `CGUIEffects` ghosting, and UI audio) with full Controller (`D-Pad`/Stick + `A`), Keyboard (`Up`/`Down`/`W`/`S` + `Enter`), and PC Mouse hover/click support for clean host shutdown without `Alt+F4` (automatically bypassed during deterministic `--replay-input`).
  - Added a thread-safe **In-Game Cheat Menu** (`F2` quick-open/close, or `F1` / `BACK + L3` -> **Cheats** tab with `LB`/`RB` controller tab switching) featuring a live Torque status header (`HP`, `Xombium`, `Insanity`, `Morality` gauge, and 1-click **Disable All Cheats**), 9 persistent combat & XBE post-FX toggles (with friendly-NPC/Dallas `#2152` protection on **One-Hit Kills**), 13 queued guest-main-thread loadout, morality, and appearance actions (including the secret **Gonzo Gun**), and an optional **`CHEATS ACTIVE (N)`** HUD badge.
- **Persistent Controller Thumbstick Inversion & In-Game Options (`[game_options]` in `settings.toml`):**
  - Fixed controller thumbstick pitch inversion (`0x002DB248`) and all 17 in-game `CUserSettings` preferences (`0x002DB218`: Controller Preset, Invert Pitch Axis, Vibration, Turn/Pitch Sensitivity, Auto-Targeting, Auto Pitch Correction, Checkpoints, Help Text, Screen Adjust X/Y, Effects/Dialog/Music Volumes, Speaker Setup, and Subtitles) resetting to Xbox factory defaults on cold restart when no Xbox `"Settings"` save container existed.
  - Added bidirectional live sync between the **Pre-Game Launcher**, **In-Game `F1` -> Controls / Audio** tabs (**Invert Controller Right-Stick Y-Axis**, **Right-Stick Turn & Pitch Sensitivity**, **Controller Rumble/Vibration**, and **In-Game Subtitles**), and the native Xbox **Options** screens, persisting every change atomically to `settings.toml` (`[game_options]`) across restarts while preserving unlocked Archive/Prelude flags (`+0x44/+0x48/+0x50`).
- **East Block, Yard & Asylum Sky/Cloud Rendering Fixes (D3D12 & Vulkan):**
  - Fixed horizontal RGB static/dark scanline bands and stepped cutouts on wall decals, outdoor concrete pillars, and scrolling multi-layer `D3DFMT_P8` sky/cloud domes across level transitions (`2 Cellblocks`, `3a YardCellBlock`, `5a Asylum`) caused by asynchronous RenderTarget readbacks writing into freed/reallocated Xbox 64 MB contiguous memory pages, sparse texture-cache `MemSig` sampling above `64 KB`, missing `MmFreeContiguousMemory` GPU cache invalidation, and un-fenced D3D12 64 MiB texture-slab suballocation reuse (plus `d3d::g_lock` / render-thread serialization on `d3d::FlushCaches()` during shutdown).
  - Fixed NV2A programmable vertex shaders (`xvsTextureMatrix` / `xvsTextureMatrix_Reflection`) having `D3DTADDRESS_WRAP` overridden to `CLAMP` when static `[0..1]` mesh UVs were scrolled or scaled via vertex shader constants `c[4..5]` (eliminating border-texel streak smearing on scrolling clouds, water, and reflections), added mid-frame `DirtyEpoch` texture re-upload and child-mip `D3DSurface` lock/busy propagation, and doubled D3D12 per-frame SRV descriptor ring capacity (`65,536` SRVs/frame).
- **Staircase Top-Edge Step-Up & Quarry Staircase Clipping / Anti-Wedge Fix:**
  - Fixed Torque getting snagged horizontally at the top lip/crest of staircases until jumping, clipping his legs through `40.5°–45°` Quarry industrial staircases (`4a/4b Quarry`), and getting locked in State `9` (`Fall`, `Jump` disabled) across unwelded `.mdu` staircase seams and V-creases — enabling `CCollider` step-up slope support (`0x8008`), fixing high-FPS (`60+ Hz`) `SweepStep` post-collision micro-slide penetration and spatial substep scaling, adding framerate-independent 3D-clearance-verified top-edge step-up assist and unwelded seam ground-bridging, and adding automatic V-crease anti-wedge recovery (bypassed during deterministic `--replay-input`).

---

## System Requirements (End Users)

### Windows (`v1.6.2` — Native Win64 D3D12)
- **OS:** Windows 11 x64 or Windows 10 1809+ x64
- **GPU:** Direct3D 12-capable GPU (Feature Level `11_0` or newer — supports NVIDIA, AMD, Intel Arc/Iris, and Handheld/iGPU UMA systems)
- **Storage:** ~400 MB free writable disk space beside `suffering.exe` (for locally compiled `suffering.dll`, per-adapter shader/PSO cache `cache\`, virtual Xbox HDD `hdd\`, and logs)

### Native Linux `x86_64` & Steam Deck (`v1.6.2` — Native Vulkan 1.2 + SDL3 + ELF64)
- **OS:** Any `x86_64` Linux distribution with **`glibc 2.35+`** and `libstdc++.so.6` (`GLIBCXX_3.4.30+`): **SteamOS 3.x (Steam Deck LCD & OLED)**, Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch Linux, Bazzite, CachyOS — **zero Proton or Wine required**, and **zero system LLVM/Clang dev packages required** on stock read-only SteamOS!
- **GPU:** Vulkan 1.2-capable GPU (AMD RADV on Steam Deck / Radeon, NVIDIA proprietary Vulkan, Intel ANV, or Mesa `lavapipe` software fallback)
- **Audio / Display / Input:** PipeWire, PulseAudio, or ALSA; Wayland, Gamescope (Steam Deck Gaming Mode), or X11; SDL3 `evdev` + `HIDAPI` (Steam Deck integrated controls, DualSense 5, Xbox, Switch) + SDL3 Relative Mouse
- **Storage:** ~400 MB free writable disk space beside `./suffering` (for locally compiled `libsuffering.so`, Vulkan shader/PSO cache `cache/`, virtual Xbox HDD `hdd/`, and logs)
- **Game Disc Image:** Your own legally dumped Original Xbox image of **The Suffering (USA)**

---

## Supported Disc Formats & Revisions

- **Target Title:** *The Suffering (USA)* — Original Xbox (`Title ID 0x4D570015` / `MW-021`, XDK `1.0.5788.1`, sole disc executable `/Default.xbe`)
- **Supported Disc Layouts (mounted read-only in place):**
  - Standard game-partition **XISO** (`.xiso.iso` / `.iso`, XDVDFS volume descriptor at offset `0x10000`)
  - Full **Redump Xbox DVD** image (`.iso`, game partition offset `0x18300000`, XDVDFS volume descriptor at `0x18310000`)
- **Supported USA Retail Revisions:**
  - `extract-xiso` XISO (`Default.xbe` SHA-1 `0a4c4e597da1f3b154099ab432c1326cc3dcb08c`)
  - Unpatched Redump XISO / DVD (`Default.xbe` SHA-1 `55c902f622047484bc6c7ea4b1b1d714da2096fc`)
- **Unsupported Formats / Regions:**
  - Compressed archives (`.7z`, `.zip`, `.rar`, `.gz`) must be extracted to `.xiso.iso` or `.iso` first.
  - PAL / non-USA regional releases are not currently supported (detected and rejected cleanly with `ERR_UNSUPPORTED_REVISION`).

---

## Quick Start (End-User Release Packages)

### Windows (`TheSuffering-Recomp-v1.6.2-win64-launcher.zip`)
You do **not** need Visual Studio, CMake, Python, or manual file extraction to play from a packaged Win64 launcher release:

1. **Unpack** the archive into any writable folder (for example, `C:\Games\TheSuffering\`).
2. **Launch `suffering.exe`** to open the **Pre-Game Graphical Launcher** (PerMonitorV2 DPI-aware Segoe UI interface with full keyboard & controller support):
   - **Select your legally owned Disc Image:** Click **Browse Disc Image...** (or let the first-launch picker guide you) to select your **The Suffering (USA)** `.xiso.iso` or `.iso`. `suffering.exe` validates it read-only (XDVDFS volume, `/Default.xbe`, Title ID `0x4D570015`, supported USA Retail SHA-1) and persists its path in `settings.toml`.
   - **Configure Hardware Preset, Upscaling, Display & GPU, Controls & Audio:** Pick one of 4 one-click Hardware Presets (**Handheld / iGPU**, **1080p Mainstream**, **1440p High**, **4K Ultra**), choose your upscaler, display adapter, and **Mouse & Keyboard / Controller** bindings, and click **PLAY**.
   - **Check for Updates & One-Click Self-Update:** The launcher checks GitHub Releases automatically in the background on startup (and anytime via **Check for Updates** in the header bar). When a new version is available, an **Update Alert** banner lets you click **Update Now** to download, verify (`SHA-256`), stage, and relaunch into the new build automatically without touching `settings.toml`, `hdd\` saves, or your disc image — or click **Open Release Page** to view full release notes.
   - **First-Launch Local Compilation:** On first launch only, `suffering.exe` uses the bundled `compiler\` tools (`xbe_recomp.exe`, `clang.exe`, `lld-link.exe`) with RAM-bounded parallelism to compile `suffering.dll` locally from your disc's `Default.xbe` with a live progress bar (typically ~15–45 seconds).
3. **Subsequent Launches & Skip-Launcher Option:**
   - `suffering.dll` is reused automatically (`compiler\work\suffering\built.txt`).
   - Enable **"Skip launcher on future starts"** inside the launcher to boot straight into gameplay on future runs.
   - To reopen the Pre-Game Launcher anytime while skipping is enabled, **hold `Shift`** while starting `suffering.exe`, pass `--launcher`, or re-enable **"Show Pre-Game Launcher on Startup"** inside the in-game **`F1`** overlay (**Launcher & Diagnostics**).

### Native Linux `x86_64` & Steam Deck (`TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz`)

Runs **100% natively on Linux Vulkan 1.2 + SDL3** with zero Proton overhead and zero system LLVM dependencies on stock read-only SteamOS:

1. **Extract** the tarball into a writable directory (for example, `~/Games/TheSuffering/`):
   ```bash
   mkdir -p ~/Games/TheSuffering
   tar -xzf TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz -C ~/Games/TheSuffering
   cd ~/Games/TheSuffering
   ```
2. **Optional — Register in Steam Deck Gaming Mode & Desktop Menu:**
   ```bash
   ./install-steamdeck.sh
   ```
   - Idempotently installs the `256x256` PNG icon, XDG `.desktop` entry (`~/.local/share/applications/the-suffering-recomp.desktop` + `~/Desktop/`), and adds **"The Suffering (Native Recomp)"** directly to Steam's `shortcuts.vdf` (backs up existing `shortcuts.vdf` to `shortcuts.vdf.bak` first; remove anytime with `./install-steamdeck.sh --uninstall`).
   - **Important:** Do **NOT** force a Proton compatibility tool in Steam Properties — this is a native Linux `ELF64` Vulkan executable!
3. **Launch `./suffering`** (or launch from Steam Gaming Mode):
   - Opens the **Pre-Game Graphical Launcher** (auto-fills `1280x800` in Gamescope on Steam Deck, auto-detects the **`Steam Deck (1280x800)`** preset with **AMD FSR 1.0 Quality `67%`** `854x480 -> 1280x720` cleanly centered in `1280x800`, 4x AF, and exact 60 FPS pacing on both 60 Hz LCD and 90 Hz OLED).
   - **Built-In Gamepad + Touch + Mouse XISO File Browser:** Click **Browse Disc Image...** to open the in-app Dear ImGui file browser with a **Quick-Jump sidebar** (`Game Folder`, `Home`, `Downloads`, `Desktop`, auto-detected Steam Deck microSD `/run/media/mmcblk0p1` & `/run/media/<user>/*`, `/`), touch-friendly rows, live read-only `ValidateDiscImage` badge, and full gamepad navigation (`D-Pad`/`Stick`, `A` open/select, `B` up, `LB`/`RB` sidebar <-> list) — zero GTK/KDE portal hangs in Gaming Mode! Desktop Linux users also get an optional **"System Dialog..."** button.
   - **First-Launch Local Compilation:** Compiles `libsuffering.so` once locally from your XISO using the bundled self-contained `compiler/{xbe_recomp,clang,ld.lld,lib/*}` with zero `/usr/include` or system packages needed.
   - **Built-In Update Checker & Self-Updater:** Includes the same **Check for Updates** header button, automatic startup update alerts, **Update Now** (`linux-x86_64.tar.gz` staging & relaunch preserving `settings.toml` and `hdd/`), and **Open Release Page** support on Desktop Linux & Steam Deck.
   - **Cross-Platform `settings.toml`:** All mouse & keyboard keybindings (`[controls.bindings]`), upscaler settings, and Xbox saves (`hdd/E/UDATA/4d570015/`) are 100% portable between Windows and Linux.

### Selecting or Relocating Your Disc Image

- **Moved / Renamed XISO:** If the saved `iso_path` can no longer be found or fails validation, `suffering.exe` reopens the Pre-Game Launcher (or a 1-click relocation prompt) with a clear error message.
- **Change XISO Anytime In-Game:** Press **`F1`** (or Controller **`BACK + L3`**) -> **Launcher & Diagnostics** -> **Relocate Disc Image...**.
- **CLI / Environment Priority Order:**
  1. `--iso "<path-to-image.xiso.iso>"` (or `--iso=<path>`)
  2. `XBE_ISO_PATH` environment variable
  3. `iso_path` in `settings.toml` beside `suffering.exe`
  4. An `.iso` / `.xiso.iso` image placed in `assets\` beside `suffering.exe`
  5. Pre-Game Launcher / first-launch native Windows file picker

---

## Modern Upscaling Suite (`v1.6.2`)

Both Windows (Direct3D 12) and Native Linux / Steam Deck (Vulkan 1.2) decouple the internal 3D rendering resolution (`render_w x render_h`) from the native display resolution (`bb_w x bb_h`), apply square-pixel aspect-locked upscaling before compositing crisp native-resolution Dear ImGui overlays, and bypass temporal upscaling cleanly on 2D menus, loading screens, and FMVs.

### All 9 Supported Upscaling Technologies

| Mode | Type | Platforms & Requirements | Notes |
| :--- | :--- | :--- | :--- |
| **AMD FSR 1.0 (EASU + RCAS)** *(Default)* | Built-in spatial compute | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | 12-tap edge-adaptive Lanczos upscale + Robust Contrast Adaptive Sharpening |
| **NVIDIA Image Scaling (NIS v1.0.3)** | Built-in spatial compute | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | 6-tap directional scaling + directional unsharp-mask sharpening |
| **AMD FidelityFX CAS** | Built-in spatial compute | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | Contrast Adaptive Sharpening & scaling (also great at 100% Native for crispness) |
| **Bicubic Catmull-Rom (5-Tap)** | Built-in spatial fragment | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | Smooth 5-tap bicubic filter; also handles >100% SSAA downsampling |
| **Pixel-Perfect Integer** | Built-in nearest fragment | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | Crisp integer-multiple pixel scaling |
| **Bilinear (Original)** | Built-in hardware filter | **Windows D3D12 & Native Linux Vulkan** (**Zero DLLs**) | Lightweight classic hardware bilinear pass |
| **NVIDIA DLSS 4 (`v310.9.16`, Transformer J/K & DLAA)** | Bundled temporal AI plugin | **Windows D3D12 Only** (NVIDIA RTX + `plugins\nvngx_dlss.dll`; gracefully falls back to FSR 1.0 on Linux Vulkan) | Supports **DLSS Preset K** (Transformer latest), **Preset J**, **Auto**, and **DLAA (100%)** |
| **AMD FSR 3.1 / 4 (`ffx_api`, SDK `v2.4.0`)** | Bundled temporal plugin | **Windows D3D12 Only** (SM 6.2+ + `plugins\amd_fidelityfx_*_dx12.dll`; gracefully falls back to FSR 1.0 on Linux Vulkan) | Exposes FSR `3.1.6` on all vendors; driver-upgradeable to FSR 4 on Radeon RX 9000 series |
| **Intel XeSS (`SDK v3.0.2` / `XeSS 2.0.2`)** | Bundled temporal AI plugin | **Windows D3D12 Only** (SM 6.4+ + `plugins\libxess.dll`; gracefully falls back to FSR 1.0 on Linux Vulkan) | High-quality temporal AI reconstruction across GPU vendors |

### Bundled Vendor Temporal Upscaler Plugins (`plugins\`)

- **End-User Release Zip (`win64-launcher.zip`):** All three official, Authenticode-signed, watermark-free vendor temporal upscaler runtimes work **out of the box** — `plugins\` pre-bundles:
  - **NVIDIA DLSS 4 (`v310.9.16`):** `plugins\nvngx_dlss.dll` + `NVNGX-DLSS-LICENSE.txt` (loaded alongside your RTX driver's `_nvngx.dll`)
  - **AMD FSR 3.1 / 4 (`SDK v2.4.0`):** `plugins\amd_fidelityfx_loader_dx12.dll` + `plugins\amd_fidelityfx_upscaler_dx12.dll` + `AMD-FIDELITYFX-LICENSE.md`
  - **Intel XeSS (`SDK v3.0.2`):** `plugins\libxess.dll` + `INTEL-XESS-LICENSE.txt` + `INTEL-XESS-THIRD-PARTY.txt`
- **Building from Source (`source.zip` / Git Clone):** Git and `source.zip` contain **zero** `.dll` binaries. Run `python tools\fetch_upscalers.py` (automatically invoked by `release.bat`) to download, SHA-256 + Authenticode verify, and stage the pinned official vendor DLLs into `out\build\release\plugins\`. Even without running `fetch_upscalers.py`, NVIDIA RTX systems automatically fall back to Authenticode-verified production DLSS models from `%ProgramData%\NVIDIA\NGX\models\dlss\` (cached in `cache\ngx\nvngx_dlss.dll`).
- **Safe Removal & Fallback:** You may update or remove any DLL in `plugins\` at any time. If a plugin is removed or unsupported on the active GPU, the engine automatically falls back to built-in **AMD FSR 1.0 (EASU + RCAS)**.

### Quality Presets, Sharpness & In-Game Sync

- **Quality Presets:** `Ultra Performance` (`33%`), `Performance` (`50%`), `Balanced` (`59%`), `Quality` (`67%`), `Ultra Quality` (`77%`), `Native / DLAA` (`100%`), `Custom (33–200%)` (values `>100%` engage Catmull-Rom SSAA downsampling).
- **Sharpness Slider (`0–100%`):** Live real-time adjustment for FSR 1.0 RCAS, NIS, CAS, FSR 3.1, and DLSS.
- **1:1 Live Sync:** Every upscaling, sharpness, aspect-correction, and display setting is shared identically between the **Pre-Game Graphical Launcher** and the **In-Game `F1` Overlay**, and the upgraded **FPS HUD** displays live `FPS | ms | Upscaler | Render -> Display` metrics.

---

## Cross-Hardware Optimizations (`v1.1.0`)

- **Multi-GPU Adapter Selection & Discrete GPU Preference:** Exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` so hybrid laptops default to the dedicated GPU, plus a full DXGI GPU Adapter selector (`Auto` or any specific discrete/integrated adapter) in both the Pre-Game Launcher and `F1` overlay.
- **4 One-Click Hardware Presets (preserve active display resolution):** **Handheld / iGPU** (Auto-Best Temporal or FSR 1.0 Quality `67%`, 4x AF, `16 MiB` UMA upload ring), **1080p Mainstream** (Auto-Best Temporal or AMD CAS `Native AA 100%` at `<=1080p` / `Ultra Quality 77%` above, 8x AF), **1440p High** (Auto-Best Temporal or AMD CAS `Native AA 100%`, 16x AF), and **4K Ultra** (Auto-Best Temporal or FSR 1.0 Quality `67%`, 16x AF).
- **Smart Presentation & Frame Pacing (`present_mode`):**
  - **Auto (Smart VSync / VRR):** Uses integer `SyncInterval` (`1..4`) on integer multiples of 60 Hz (`60/120/180/240 Hz`) and switches automatically to `DXGI_PRESENT_ALLOW_TEARING` on VRR / non-60-multiple monitors (`75/90/144/165 Hz`) with VSync-aware `Pace()` coordination to prevent double-wait hitching.
  - Manual overrides: **VSync**, **VRR / Allow Tearing**, and **Tear-Free Mailbox**.
- **Adaptive D3D12 Memory & Per-Adapter PSO Cache:** Probes Feature Level (`11_0+`), Shader Model, UMA architecture, and DXGI VRAM budget (`QueryVideoMemoryInfo`), dynamically sizing the upload ring (`16 / 32 / 64 MiB`) and texture eviction thresholds. Stores PSO caches per adapter (`cache\psos_<VID>_<DID>.bin`) with automatic retry-storm protection on Wine / vkd3d-proton.
- **Hybrid CPU P-Core Affinity & RAM-Bounded Compiler:** Single-core guest pinning automatically selects a high-efficiency P-core while avoiding Physical Core 0 (and its SMT sibling) to eliminate OS/DPC jitter. First-launch local compilation bounds parallel `clang` workers by available system RAM and commit limit (overrideable via `--compile-jobs` / `XBE_COMPILE_JOBS`) so 8–16 GB laptops and handhelds never OOM.
- **PerMonitorV2 DPI Scaling & Wine/Proton Compatibility:** Embedded `PerMonitorV2` manifest with crisp DPI-scaled Segoe UI fonts, `WM_DPICHANGED` handling, `FILE_SHARE_READ | FILE_SHARE_WRITE` disc handles, 64 KiB buffered file I/O, `XAUDIO2_USE_DEFAULT_PROCESSOR`, and automatic Wine/Proton/Steam Deck XInput compatibility mode (toggleable in Controller settings).

---

## In-Game Settings & Cheat Overlay (`F1` / `F2` or `BACK + L3`)

Press **`F1`** on the keyboard (or **`BACK + Left Stick Click (L3)`** on a controller; **`LB` / `RB`** switches tabs; **`ESC`** closes) to open the thread-safe DPI-scaled Dear ImGui overlay, or press **`F2`** anytime to open directly to (or switch to / close) the **Cheats** tab:

- **Display & GPU:** GPU Adapter selector, Windowed / Borderless / Fullscreen modes, Monitor & Screen Resolution selector (`16:9` & `16:10`), Square-Pixel Aspect Correction, Presentation Mode (`Auto`, `VSync`, `VRR / Allow Tearing`, `Tear-Free Mailbox`), Anisotropic Filtering (`Game Default`–`16x`), Safe Frame-Rate Cap (`60 FPS` vs `30 FPS`), and Hardware Presets.
- **Upscaling:** Live switching across all 9 upscalers (with per-plugin availability badges & fallback status), Quality Preset / Custom Render Scale (`33–200%`), Sharpness (`0–100%`), DLSS Preset (`Auto`, `K`, `J`, `E`), and live `Render -> Display` resolution readout.
- **Controls (Mouse & Keyboard + Controller):**
  - **Input Mode:** `Auto (Mouse/KB + Controller - Instant Hot-Switch)` *(default)*, `Mouse & Keyboard Only`, or `Controller Only` — automatically enables zero-controller keyboard/mouse play from launch and hot-switches instantly between mouse look and right-stick look.
  - **Mouse & Keyboard Sub-Tab:** Sensitivity slider (`0.10–5.00x`, with live `cm/360 @ 800 DPI` readout), **Invert Mouse Y** (decoupled from the Xbox controller pitch setting), **Raw 1:1 Mouse Aim** (bypasses controller sticky aim-slowdown on targets), optional **Mouse Smoothing**, **Reset to PC Defaults**, and an interactive 4-column **Click-to-Rebind** table (`Primary` + `Secondary` slots across all 18 actions supporting keyboard keys, mouse buttons `LMB/RMB/MMB/Mouse4/Mouse5`, and `Wheel Up`/`Wheel Down`; `Esc` cancels, `Del` clears).
  - **Controller Sub-Tab:** Live Port 1 SDL3 / DualSense 5 device status (`VID:PID`), **Invert Controller Right-Stick Y-Axis** + **Right-Stick Turn & Pitch Sensitivity** (`0.25–4.00x`, synced 1:1 with in-game Xbox Options and persisted in `settings.toml` `[game_options]`), Rumble enable + strength (`0–100%`) + Test Pulse, Extra Host Stick Deadzone (`0–30%`), DualSense Touchpad Click -> Xbox `BACK` toggle, and Wine / Steam Deck XInput Compatibility toggle.
- **Audio:** Master Volume (`0–100%`), Mute All, Mute When Unfocused, **Enable In-Game Subtitles** (synced 1:1 with `[game_options]`), and Speaker Output Mode (`Auto`, `Stereo`, `5.1 Surround`, `Mono`).
- **Launcher & Diagnostics:** **Updates** section (**Check for Updates**, **Automatically check for updates on startup** toggle, background corner toast notification when skipping the launcher, and **Save & Exit to Update** / **Open Release Page** controls), live FPS / Frame-Time / Upscaler HUD toggle, **Show Pre-Game Launcher on Startup** toggle, per-adapter Shader/PSO Cache stats & 1-click **Reset Shader & PSO Cache**, **Relocate Disc Image...**, **Open Save Data / Logs / CrashDumps / Plugins** folders, and **Create Desktop Shortcut**.
- **Cheats (`F2` Quick-Toggle):** Dedicated Dark-Industrial cheat suite (automatically suppressed during deterministic `--replay-input` / autotest runs):
  - **Live Status Header Card:** Displays real-time Torque telemetry (`HP`, `Xombium`, `Insanity %`, `Morality` tier & score, plus clip/reserve ammo & flashlight battery tooltip) when in an active level, a clear notice on the Main Menu (*persistent toggles can be armed anytime*), live action execution feedback, and a 1-click **Disable All Cheats** button whenever toggles are active.
  - **Survival, Combat & Visual Toggles (9 Persistent Toggles):**
    - **God Mode (Infinite Health):** Zero damage from enemy attacks, fire, explosions, falls, and Insanity-overstay health drain.
    - **Infinite Reserve Ammo:** Reserve ammo never depletes on reload and stays topped up across all owned firearms.
    - **Bottomless Clip (No Reload):** Active weapon magazine and flamethrower fuel stay 100% full (including dual-wield).
    - **Infinite Items & Throwables:** Xombium bottles, Shrapnel Grenades, Molotovs, Dynamite, and Flares never decrease when used.
    - **Infinite Flashlight Battery:** Full-beam flashlight stays at 100% intensity and never dims or drains.
    - **Infinite Insanity:** Insanity meter stays 100% ready and Beast transformation never drains over time.
    - **One-Hit Kills (Hostiles Only):** Hostile enemies damaged by Torque die instantly while strictly protecting allies/escorts (including Dallas `#2152`), scripted invulnerable NPCs, and props/doors.
    - **Psychedelic Mode (`"Tripping"`):** Built-in XBE *"Tripping the Light Fantastic"* rainbow-trail post-processing filter at normal `1.0x` game speed.
    - **Old Movie Projector (`"Murnau"`):** Built-in XBE *"Murnau Mode"* vintage black-and-white film grain & projector scratches filter.
  - **Instant Loadout & Player Actions (Active Level + Live Torque):** **Full Heal + Max Xombium** (`9/9`), **Give All Weapons & Max Ammo**, **Refill All Throwables & Items**, **Fill Insanity Meter**, **Max All Firearm Ammo**, and **Give Gonzo Gun (Secret)** (grants the hidden chicken-firing Gonzo Gun).
  - **Morality & Bonus Effects:** Live Evil-to-Good reputation gauge with **Set Max Evil**, **Shift Evil (`-50`)**, **Reset Neutral (`0`)**, **Shift Good (`+50`)**, and **Set Max Good**, plus XBE appearance one-shots (**Bloodsoaked Torque** and **Clean Torque**).
  - **Subtle HUD Indicator:** Optional **"Show active cheats badge on HUD"** toggle (`show_cheats_hud` in `settings.toml`, enabled by default when cheats are armed; hidden in captures/replays) displaying a compact `CHEATS ACTIVE (N)` corner pill.

### Desktop Shortcut & Utility CLI Flags

```powershell
# Create a Windows Desktop shortcut (or specify a custom target folder)
suffering.exe --create-shortcut
suffering.exe --create-shortcut "C:\MyShortcuts"

# Non-interactive XISO validation (prints JSON summary, exit 0 = valid / 1 = invalid)
suffering.exe --validate-iso "D:\Games\Suffering, The (USA).xiso.iso"

# Force Pre-Game Launcher open (even if skip-launcher is enabled) or bypass it directly
suffering.exe --launcher
suffering.exe --no-launcher
```

Additional runtime flags (non-persistent session overrides unless saved via UI):
- `--upscaler bilinear|integer|bicubic|cas|nis|fsr1|dlss|fsr3|xess`, `--upscale-quality native_aa|ultra_quality|quality|balanced|performance|ultra_performance|custom`, `--render-percent 33..200`, `--sharpness 0..100`, `--dlss-preset auto|e|j|k`
- `--present-mode auto|vsync|vrr|mailbox`, `--gpu-adapter auto|<VID:DID:SubSys:Ord>|<name>`, `--hw-preset handheld|1080p|1440p|4k`, `--aspect-correct 0|1`
- `--input-mode auto|mkb|controller`, `--mouse-sens 0.10..5.00`, `--mouse-invert-y 0|1` — Mouse & Keyboard session overrides (non-persistent unless saved via UI)
- `--fps-cap 30|60` (`XBE_FPS_CAP`), `--vsync 0|1` (`XBE_VSYNC`), `--compile-jobs <N>` (`XBE_COMPILE_JOBS`)
- `--open-settings`, `--settings-tab 0..5|cheats`, `--open-cheats` (`XBE_OPEN_CHEATS=1`) — open the in-game Settings or Cheats overlay tab on launch
- `--no-shader-cache` (`XBE_NO_SHADER_CACHE=1`) — disable persistent DXBC/PSO cache for diagnostics
- `--perf-log <path.json>` (`XBE_PERF_LOG`) — opt-in 11-metric performance telemetry (`.json` + `.frames.csv`)
- `--record-input <file.rec>`, `--replay-input <file.rec>`, `--replay-mode hybrid|frame|time`, `--replay-exit`, `--state-log <file.jsonl>` — deterministic input recording & regression replay
  Recording (`--record-input` or `XBE_INPUT_RECORD`) uses controller input only: native mouse aim, keyboard gameplay input, menu mouse actions, host guest-option overrides, cheats, and native stair assists are disabled for that session to match replay behavior. Connect a gamepad, or use the scripted controller autotest. A recording notice stays visible in-game; saved input preferences are unchanged. The existing `#TSREC 1` format and older controller recordings remain supported; native mouse/keyboard sessions cannot be recorded in this format.

---

## Native-PC Mouse & Keyboard Controls (`v1.1.0`)

*The Suffering* supports full **zero-controller, native-PC Mouse & Keyboard play** from launch to end-credits alongside instant **Auto hot-switching** with DualSense 5 / Xbox controllers:

- **True 1:1 Raw Mouse Aim:** Uses Windows Raw Input (`WM_INPUT`) injected directly past the Original Xbox 30% stick deadzone, divided algebraically by frame `dt` and guest sensitivity for framerate-independent 1:1 horizontal & vertical camera control (`~0.09°/count` at `1.00x` across 3rd-Person, 1st-Person, Creature, and Mounted Turret/Searchlight modes), with yaw slew acceleration bypassed for **zero stop-coasting drift** and optional **Raw 1:1 Aim** bypassing controller sticky target slowdown.
- **Full Point-and-Click Menu, Main Menu `"Quit Game"` & Interactive Map Mouse Support (`v1.4.0`–`v1.6.2`):** Hover to highlight items and Left-Click (`LMB`) to select across Title, Main Menu (including the native **Quit Game** option directly below **Bonus Material** for clean PC exit without `Alt+F4`), Pause, Options (including clicking/dragging sliders and toggles, automatically persisted to `settings.toml`), Save/Load lists, Clem's Journal/Archives, and Confirmation modals; Right-Click (`RMB`) to go Back/Unpause; scroll the **Mouse Wheel** through menus and journal pages; and **Left-Click Drag + Mouse Wheel Zoom** on the interactive Map (`Tab` / `M`) — with automatic click-release latching so clicking *Resume Game* never fires a stray shot.
- **Smart Cursor Capture & Release:** Automatically clips and hides the cursor during active 3D gameplay, and releases it immediately on Pause, Archives/Map, FMVs/Cutscenes, Title/Main Menu, `F1` / `F2` Overlay, `Alt+Tab`/Focus Loss, or window resize/move.
- **Scheme 1 Flashlight Upgrade:** Automatically upgrades the game's default Xbox control table (`Scheme 0`) to `Scheme 1` (`R3` -> Flashlight Toggle) so **`F`** toggles the flashlight natively on 1 keypress without disturbing controller layouts.
- **All 18 Actions Rebindable (`Primary` + `Secondary`):** Stored under `[controls]` and `[controls.bindings]` in `settings.toml` and synced live between the Pre-Game Launcher (**Controls** tab) and the in-game **`F1` -> Controls** tab.

| Action | Default Primary | Default Secondary | Notes |
| :--- | :--- | :--- | :--- |
| **Move Forward / Back / Strafe Left / Right** | `W` / `S` / `A` / `D` | — | Diagonal-normalized (`magnitude = 1.000`) |
| **Aim / Look** | **Mouse (Raw Input)** | — | 1:1 yaw & pitch, zero deadzone, zero coasting |
| **Fire / Attack** | `LMB` (`MouseLeft`) | — | Right Trigger (`RT`) |
| **Throw Grenade / Creature Swipe** | `RMB` (`MouseRight`) | `G` | Left Trigger (`LT`) |
| **Jump / Leap / Dismount** | `Space` | — | Also confirms/skips in menus & FMVs |
| **Reload** | `R` | — | Xbox `B` during gameplay |
| **Use / Interact (Hold: Push/Pull Crate)** | `E` | — | Xbox `X` |
| **Flashlight Toggle** | `F` | `L` | Dedicated Scheme 1 `R3` toggle |
| **Heal (Xombium Bottle)** | `Q` | `H` | Xbox `Black` |
| **Insanity / Creature Transform** | `X` | `Mouse4` | Xbox `Y` |
| **1st / 3rd Person Camera Toggle** | `C` | `MMB` (`MouseMiddle`) | Xbox `White` |
| **Archives / Map / Journal** | `Tab` | `M` | Xbox `Back` |
| **Inventory Category Up / Down** | `1` / `2` | `ArrowUp` / `ArrowDown` | D-Pad Up / Down |
| **Previous / Next Weapon or Item** | `WheelUp` / `WheelDown` | `3` / `4` | Queued D-Pad Left / Right notch pulses |
| **Pause / Menu Back / Skip Cutscene** | `Esc` | — | Context-aware latched (`Start` in 3D, `B` in Pause/Archives/Menus, `A` during FMVs/Cutscenes) |
| **Menu Confirm / Back / Delete Save** | `Enter` / `Backspace` / `Del` | `Arrow Keys` / `WASD` | Active automatically outside 3D gameplay (incl. Main Menu **Quit Game**) |
| **Settings / Cheat Menu Overlay** | `F1` / `F2` | — | `F1` opens/closes overlay; `F2` jumps straight to **Cheats** |

---

## Controller Configuration (DualSense 5 & Xbox via SDL3)

Controller input is handled statically via **SDL3 `3.4.16` (HIDAPI + WGI)** with hot-plug support for PlayStation DualSense 5 (`VID 0x054C / PID 0x0CE6`), DualSense Edge (`0x0DF2`), Xbox One / Series controllers, and Switch Pro controllers over USB or Bluetooth.

| Original Xbox Control | PlayStation DualSense 5 | Xbox Controller | Notes |
| :--- | :--- | :--- | :--- |
| **A / B / X / Y** | Cross / Circle / Square / Triangle | A / B / X / Y | Full pressure/digital mapping |
| **White / Black** | L1 / R1 | LB / RB | Also navigates ImGui overlay tabs |
| **Left / Right Trigger** | L2 / R2 | LT / RT | Analog 0–255 triggers |
| **START / BACK** | Options / Create (or Touchpad Click) | Menu / View | Touchpad -> `BACK` toggleable in Settings |
| **L3 / R3 / D-Pad / Sticks** | L3 / R3 / D-Pad / Analog Sticks | LS / RS / D-Pad / Sticks | Game has built-in 30% deadzone; optional extra 0–30% in Settings |
| **Settings & Cheats Overlay** | **Create/Touchpad + L3** (or `F1` / `F2`) | **View + LS** (or `F1` / `F2`) | Blocks game input while open; `LB` / `RB` switches tabs (incl. **Cheats**) |

- **Custom Mappings:** Place an SDL `gamecontrollerdb.txt` beside `suffering.exe` to add custom controller mappings automatically at startup.
- **Rumble:** Fixed SDL3 sustained motor keepalive (`65535 ms` hold until zeroed), with toggle, 0–100% intensity slider, and 300 ms Test Pulse inside **Settings -> Controls**.

---

## Known Compatibility & Performance Notes

- **60 FPS Default + Unlocked High-Refresh (`90 / 120 / 165+ FPS`) Support:** Original Xbox *The Suffering* targeted `D3DPRESENT_INTERVAL_ONE` (60 Hz VSync-1 at `0x001A26A0`), and its cloth solver (`0x001A6480`) clamped `dt` to `[0.016s, 0.030s]`. A monotonic high-precision `CClock` accumulator (`0x002DC724`) and a 60 Hz fixed-step cloth accumulator hook keep `1.00x` simulation & physics stability across `30 FPS`, `60 FPS`, `90 FPS` (Steam Deck OLED), `120 FPS`, `165 FPS`, and `Uncapped` modes.
- **Startup Shader & PSO Pre-Compilation + Persistent Disk Cache:** At boot (before Frame 1), both D3D12 (Windows) and Vulkan (Linux / Steam Deck) pre-compile all built-in blit/clear/depth/particle pipelines, common fixed-function UI/3D FVF & combiner permutations, any previously recorded `cache/shader_recipes.bin` recipes, and every guest programmable vertex/pixel shader loaded from `ShaderLibrary.bin` & `xpsBW.xpu` in parallel across up to 12 worker threads — then pre-build all cached graphics PSOs (`pso_meta_*` / `vk_pso_meta_*`) across GPU driver updates and defer heavy pipeline-library serialization away from active 3D gameplay.
- **Single-Core Guest Scheduling (`single_core_guest = true`):** Guest Xbox threads are pinned to one logical CPU core with authentic guest thread priorities while host D3D12 rendering, XAudio2 audio, SDL3 input, and Dear ImGui remain multi-core. This prevents Original Xbox single-core race conditions (`0x00176712` bitfield RMW and `0x2E1444 <-> 0x2E1A90` AB-BA lock order) with zero performance penalty.
- **Hardware Audit Note:** DualSense 5 HIDAPI driver detection, mapping, and sustained rumble code paths are verified statically and at runtime; live physical USB/Bluetooth DualSense 5 touch-testing awaits plugging in a physical controller.

---

## Troubleshooting & Recovery

All runtime data is portable and stored beside `suffering.exe`:

| Purpose | Path | Recovery Action |
| :--- | :--- | :--- |
| Settings | `settings.toml` | Delete to reset all display/audio/input settings and re-open the first-launch XISO picker. |
| Xbox Save Data (`U:\`) | `hdd\E\UDATA\4d570015\` | Back up or restore this folder directly to transfer save games. |
| Xbox Scratch Cache (`Z:\`) | `hdd\Z\` | Safe to delete anytime; the game rebuilds cached level archives automatically from the XISO. |
| Shader & PSO Disk Cache | `cache\shaders.bin`, `cache\shader_recipes.bin`, `cache\psos_<VID>_<DID>.bin` | Click **F1 -> Launcher & Diagnostics -> Reset Shader & PSO Cache** (or delete `cache\`) after a GPU driver update or visual artifact. |
| Bundled Upscaler Plugins | `plugins\` | Contains the verified official `nvngx_dlss.dll`, `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll`, and `libxess.dll` plus their vendor licenses (see `plugins\README.txt`). Safe to update or delete anytime. |
| Compiled Game Code | `suffering.dll`, `compiler\work\` | Delete `suffering.dll` and `compiler\work\` to force a clean local recompilation on next launch. |
| Logs & Crash Dumps | `logs\suffering.log`, `logs\compile.log`, `CrashDumps\*.dmp` | Inspect or attach these files when diagnosing startup, compilation, or runtime issues. |

**Common XISO Validation Errors:**
- `ERR_COMPRESSED_ARCHIVE`: You selected a `.7z`/`.zip`/`.rar`/`.gz` archive — extract the `.xiso.iso` or `.iso` first.
- `ERR_NOT_XISO`: File is missing the `MICROSOFT*XBOX*MEDIA` XDVDFS descriptor (corrupted dump or non-Xbox ISO).
- `ERR_WRONG_GAME`: Disc image is a different Xbox title (expected Title ID `0x4D570015`).
- `ERR_UNSUPPORTED_REVISION`: Disc image is a non-USA/unrecognized revision of *The Suffering*.

---

## Building from Source (Developers)

### Windows (`x64` Direct3D 12)
- **Host Requirements:** Windows 10/11 x64, VS 2022/2026 Build Tools (C++ workload + Windows SDK), LLVM for Windows (`clang`, `clang++`, `lld-link`), CMake 3.25+, Ninja, Git, Python 3.11+, 7-Zip (`7z.exe`, for `release.bat`).
- **Build & Package:**
  ```bat
  set "XBE_ISO_PATH=C:\Path\To\Suffering, The (USA).xiso.iso"
  build.bat --no-prompt
  release.bat
  ```
  *(Pass `-DSUFFERING_ALLOW_NO_XBE=ON` to CMake to compile the host executable and compiler tools without a local XBE/ISO — used by GitHub Actions CI.)*

### Native Linux `x86_64` (`Vulkan 1.2 + SDL3 + ELF64`)
- **Host Requirements (Ubuntu 22.04 recommended for `glibc 2.35` compatibility):**
  ```bash
  sudo apt-get install -y clang lld cmake ninja-build patchelf pkg-config python3 \
    glslang-dev spirv-tools libvulkan-dev zlib1g-dev \
    libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libxss-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev
  ```
- **Bootstrap, Build & Package:**
  ```bash
  bash setup_sdk.sh
  # Build without local XBE/ISO (compiles ./suffering + ./compiler/xbe_recomp for first-launch compilation):
  cmake --preset linux-release -DSUFFERING_ALLOW_NO_XBE=ON
  cmake --build out/build/linux-release --parallel
  # Package self-contained Linux & Steam Deck tarball:
  bash release_linux.sh --build-dir out/build/linux-release \
    --out-tar .local/dist/TheSuffering-Recomp-v1.6.2-linux-x86_64.tar.gz
  ```
- **Automated 6-Gate Release & Copyright Audit (Windows + Linux ELF64):**
  ```bash
  python3 -u tools/audit_release.py --allow-remote
  ```
- **Additional Verification Suites:**
  ```powershell
  python -u tools\test_review_fixes.py                   # Asset-free review-fix regression suite (gameplay, input, renderer/SDK, updater)
  python -u tools\test_real_upscalers.py                 # Live real DLSS 4, FSR 3.1, XeSS & NGX driver fallback suite
  python -u tools\test_mkb_controls.py                   # Native-PC Mouse & Keyboard gameplay, aim, cursor & UI suite
  python -u tools\test_v11_launcher_upscalers.py         # Pre-Game Launcher, 9-upscaler & cross-hardware suite
  python -u tools\test_m5_launcher.py                    # Milestone 5 validator, release.bat & ImGui suite
  python -u tools\run_regression.py                      # Milestone 3 9-pass deterministic gameplay regression
  python -u tools\run_perf_benchmark.py --phase postopt  # Milestone 4 11-metric performance benchmark
  ```
